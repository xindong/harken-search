from __future__ import annotations

import contextlib
import io
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import platform_boundary_check as boundary

ALLOWED_OWNER_SLICES = {
    "S6-02", "S6-03", "S6-04", "S6-05A", "S6-05B", "S6-05C", "S6-06", "S6-07A", "S6-07B",
}


class Stage6PlatformBoundaryContract(unittest.TestCase):
    def test_windows_main_com_startup_uses_thread_com_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("int WINAPI WinMain", 1)[1]
        self.assertIn("Platform::InitThreadCom();", body)
        self.assertNotIn("CoInitializeEx(", body)

    def test_windows_main_appdata_lookup_uses_system_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.rsplit("const auto layout = Platform::HarkenUserData::EnsureLayout();", 1)[1].split(
            "io.IniFilename", 1
        )[0]
        self.assertIn("layout.activeSettingsDir", body)
        self.assertIn("Platform::HarkenUserData::WideToUtf8Path(layout.activeSettingsDir)", body)
        self.assertNotIn("SHGetFolderPathA", body)

    def _fixture(self, files: dict[str, str]) -> Path:
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        root = Path(temp_dir.name)
        for relative_path, contents in files.items():
            path = root / relative_path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
        return root

    def _scan(self, files: dict[str, str]) -> list[boundary.Finding]:
        return boundary.scan_tree(self._fixture(files))

    @staticmethod
    def _rules(findings: list[boundary.Finding]) -> set[str]:
        return {finding.rule for finding in findings}

    def test_database_audio_probe_native_free_header_contract(self) -> None:
        public_path = ROOT / "src/platform/database_audio_probe.h"
        private_path = ROOT / "src/platform/database_audio_probe_backend.h"
        header_test_path = ROOT / "tests/test_database_audio_probe_header.cpp"
        for path in (public_path, private_path, header_test_path):
            self.assertTrue(path.is_file(), f"missing required contract file: {path}")

        public = public_path.read_text(encoding="utf-8")
        for declaration in (
            "namespace Platform::DatabaseAudioProbe", "uint32_t sampleRate = 0;",
            "uint32_t channels = 0;", "uint64_t totalFrames = 0;",
            "float durationSeconds = 0.0f;",
            "bool Probe(const std::filesystem::path& path, Info& out);"):
            self.assertIn(declaration, public)
        for forbidden in (
            "miniaudio", "MediaFoundation", "PathToUtf8", "windows.h",
            "_WIN32", "AURO_DATABASE_AUDIO_PROBE_TEST", "HANDLE", "__try"):
            self.assertNotIn(forbidden, public)
        self.assertNotRegex(public, r"\bma_")

        private = private_path.read_text(encoding="utf-8")
        self.assertIn('#include "platform/database_audio_probe.h"', private)
        self.assertIn("namespace Platform::DatabaseAudioProbe::Detail", private)
        for declaration in (
            "bool CanDecodeExtension(const std::string& pathUtf8);",
            "bool IsFlacExtension(const std::string& pathUtf8);",
            "bool ProbeMediaFoundation(const std::string& pathUtf8, Info& out, std::string& error);",
            "bool ProbeMiniaudio(const std::filesystem::path& path, Info& out);"):
            self.assertIn(declaration, private)

        header_test = header_test_path.read_text(encoding="utf-8")
        self.assertEqual(1, header_test.count("#include"))
        self.assertIn('#include "platform/database_audio_probe.h"', header_test)

        plan = (ROOT / "docs/superpowers/plans/2026-07-20-stage6-s6-05b-database-audio-metadata-probe.md").read_text(encoding="utf-8")
        for later_file in (
            "src/platform/database_audio_probe.cpp",
            "src/platform/database_audio_probe_miniaudio.cpp",
            "src/platform/database_audio_probe_media_foundation.cpp",
            "tests/database_audio_probe_path_utf8_adapter.cpp",
            "tests/test_database_audio_probe_routing.cpp",
            "tests/test_database_audio_probe_backend.cpp",
            "tests/test_database_audio_probe_integration.cpp"):
            self.assertIn(later_file, plan)

        behavior = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
        block = behavior.split("add_executable(TestDatabaseAudioProbeHeaderSelfContained", 1)[1].split(
            "add_test(NAME TestDatabaseAudioProbeHeaderSelfContained", 1)[0]
        source_list = block.split(")", 1)[0]
        self.assertEqual("EXCLUDE_FROM_ALL\n    tests/test_database_audio_probe_header.cpp", source_list.strip())
        self.assertIn('set_property(TARGET TestDatabaseAudioProbeHeaderSelfContained PROPERTY LINK_LIBRARIES "")', block)
        includes = block.split(
            "target_include_directories(TestDatabaseAudioProbeHeaderSelfContained", 1
        )[1].split(")", 1)[0]
        self.assertIn("${CMAKE_CURRENT_SOURCE_DIR}/src", includes)
        self.assertNotIn("${CMAKE_CURRENT_SOURCE_DIR}/tests", includes)
        for forbidden in (
            "database_audio_probe.cpp", "database_audio_probe_miniaudio.cpp",
            "database_audio_probe_media_foundation.cpp", "miniaudio_impl.cpp",
            "AudioEngine.cpp"):
            self.assertNotIn(forbidden, source_list)
        self.assertNotIn("target_compile_definitions(TestDatabaseAudioProbeHeaderSelfContained", block)
        self.assertIn("_auro_assert_link_library_count(TestDatabaseAudioProbeHeaderSelfContained 0)", block)
        self.assertIn('_auro_assert_include_count(${_target} "${CMAKE_CURRENT_SOURCE_DIR}/tests" 0)', block)
        for macro in ("AURO_DATABASE_AUDIO_PROBE_TEST_WIN32",
                      "AURO_DATABASE_AUDIO_PROBE_TEST_STD",
                      "AURO_DATABASE_AUDIO_PROBE_TESTING"):
            self.assertIn(macro, block)
        self.assertIn("_auro_assert_compile_definition_count(${_target} ${_definition} 0)", block)

    def test_database_audio_probe_routing_topology(self) -> None:
        required = (
            "tests/test_database_audio_probe_routing.cpp",
            "tests/database_audio_probe_scripted_backend.h",
            "tests/database_audio_probe_scripted_backend.cpp",
            "tests/database_audio_probe_path_utf8_adapter.cpp",
        )
        for relative in required:
            self.assertTrue((ROOT / relative).is_file(), f"missing routing RED file: {relative}")

        scripted = (ROOT / required[1]).read_text(encoding="utf-8")
        for token in ("kCapacity", "QueueCanDecode", "QueueIsFlac",
                      "QueueMediaFoundation", "QueueMiniaudio", "Snapshot Get"):
            self.assertIn(token, scripted)
        adapter = (ROOT / required[3]).read_text(encoding="utf-8")
        self.assertIn('#include "platform/text_encoding.inl"', adapter)
        self.assertIn("std::string PathToUtf8(const std::filesystem::path& path)", adapter)
        self.assertIn("Platform::TextEncoding::WideToUtf8(path.wstring())", adapter)
        self.assertIn("return path.string();", adapter)
        public = (ROOT / "src/platform/database_audio_probe.h").read_text(encoding="utf-8")
        self.assertNotIn("PathToUtf8", public)

        behavior = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
        for target, macro in (
            ("TestDatabaseAudioProbeRoutingWin32", "AURO_DATABASE_AUDIO_PROBE_TEST_WIN32=1"),
            ("TestDatabaseAudioProbeRoutingStd", "AURO_DATABASE_AUDIO_PROBE_TEST_STD=1"),
        ):
            block = behavior.split(f"add_executable({target}", 1)[1].split(
                f"add_test(NAME {target}", 1)[0]
            self.assertIn("tests/test_database_audio_probe_routing.cpp", block)
            self.assertIn("tests/database_audio_probe_scripted_backend.cpp", block)
            self.assertIn(macro, block)
            self.assertIn(f'set_property(TARGET {target} PROPERTY LINK_LIBRARIES "")', block)
            self.assertIn("_auro_assert_link_library_count(${_target} 0)", block)
            sources = block.split(")", 1)[0]
            self.assertEqual(1, sources.count("src/platform/database_audio_probe.cpp"))
            self.assertEqual(target.endswith("Win32"), "database_audio_probe_path_utf8_adapter.cpp" in sources)
            self.assertEqual(target.endswith("Win32"), "AURO_DATABASE_PROBE_ROUTING_TEXT_SOURCE" in sources)
        self.assertIn("set(AURO_DATABASE_PROBE_ROUTING_TEXT_SOURCE src/platform/text_encoding_win32.cpp)", behavior)
        self.assertIn("set(AURO_DATABASE_PROBE_ROUTING_TEXT_SOURCE src/platform/text_encoding_std.cpp)", behavior)
        self.assertIn("_auro_assert_source_count(HarkenSearch database_audio_probe_path_utf8_adapter.cpp 0)", behavior)
        self.assertIn('_auro_assert_source_count(HarkenSearch "${_auro_audio_selected_text}" 1)', behavior)
        self.assertIn('_auro_assert_source_count(HarkenSearch "${_auro_audio_unselected_text}" 0)', behavior)
        orchestration = (ROOT / "src/platform/database_audio_probe.cpp").read_text(encoding="utf-8")
        for diagnostic in ("MediaFoundationProbeOK: path=%s format=%s sr=%u ch=%u frames=%llu dur=%.4f",
                           "MediaFoundationProbeFailed: path=%s format=%s error=%s", "fallback=flac"):
            self.assertIn(diagnostic, orchestration)
        for policy in ("defined(AURO_DATABASE_AUDIO_PROBE_TEST_WIN32) && defined(AURO_DATABASE_AUDIO_PROBE_TEST_STD)",
                       "#error Database audio probe test policies are mutually exclusive"):
            self.assertIn(policy, orchestration)
        self.assertRegex(orchestration, r"out\s*=\s*\{\};\s*if\s*\(\s*TryWave\(path,\s*out\)\s*\)")
        root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        app_sources = root_cmake.split("set(APP_SOURCES", 1)[1].split(")", 1)[0]
        self.assertNotIn("database_audio_probe_path_utf8_adapter.cpp", app_sources)

    def test_database_delegates_audio_metadata_probe_ownership(self) -> None:
        database = (ROOT / "src/Database.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "platform/database_audio_probe.h"', database)

        wrapper = database.split("static bool readAudioFileInfo(", 1)[1].split(
            "bool Database::probeAudioFileInfo", 1
        )[0]
        self.assertIn("Platform::DatabaseAudioProbe::Info info;", wrapper)
        self.assertIn(
            "const bool ok = Platform::DatabaseAudioProbe::Probe(path, info);",
            wrapper,
        )
        for field in ("sampleRate", "channels", "totalFrames", "durationSeconds"):
            self.assertIn(f"{field} = info.{field};", wrapper)
        self.assertIn("return ok;", wrapper)

        public_probe = database.split("bool Database::probeAudioFileInfo", 1)[1].split(
            "}", 1
        )[0]
        self.assertIn("return readAudioFileInfo(Utf8ToPath(filePath)", public_probe)
        for forbidden in (
            'MediaFoundationAudioDecoder.h', "miniaudio.h",
            "readAudioFileInfoImpl", "readAudioFileInfoSEH",
            "MediaFoundationAudio::", "ma_decoder", "ma_result",
            "ma_uint32", "ma_uint64", "ma_format_f32",
        ):
            self.assertNotIn(forbidden, database)

        root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        app_sources = root_cmake.split("set(APP_SOURCES", 1)[1].split(")", 1)[0]
        for source in (
            "src/platform/database_audio_probe.cpp",
            "src/platform/database_audio_probe_miniaudio.cpp",
            "src/platform/database_audio_probe_media_foundation.cpp",
        ):
            self.assertEqual(1, app_sources.count(source))
        for forbidden in (
            "database_audio_probe_scripted_backend.cpp",
            "database_audio_probe_miniaudio_test_seam.cpp",
            "tests/miniaudio_impl.cpp",
        ):
            self.assertNotIn(forbidden, app_sources)

    def test_database_drops_unused_windows_headers_but_keeps_copyfile_guard(self) -> None:
        database = (ROOT / "src/Database.cpp").read_text(encoding="utf-8")
        for unused_native_header in ("windows.h", "dbghelp.h"):
            self.assertNotIn(f"#include <{unused_native_header}>", database)
        self.assertIn(
            "#ifdef _WIN32\n#ifdef CopyFile\n#undef CopyFile\n#endif\n#endif",
            database,
        )

    def test_database_cover_perf_log_uses_stream_writer_pal(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = database.split("static void AppendCoverPerfLog", 1)[1].split(
            "// Register the C++ text helpers", 1
        )[0]
        self.assertIn("Platform::FileSystem::StreamWriter", body)
        self.assertIn("Platform::FileSystem::TemporaryDirectory()", body)
        self.assertIn("writer.Open(logPath, true, false)", body)
        self.assertIn("writer.Write(line + \"\\n\")", body)
        self.assertIn("writer.Flush()", body)
        self.assertIn("writer.Close()", body)
        for direct_io in ("FILE*", "fopen_s", "fopen(", "fprintf(", "fflush(", "fclose("):
            self.assertNotIn(direct_io, body)

    def test_database_playlist_export_uses_text_file_pal(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = database.split("bool Database::exportPlaylist", 1)[1].split(
            "int64_t Database::createSmartPlaylist", 1
        )[0]
        self.assertIn("std::ostringstream content;", body)
        self.assertIn("Platform::FileSystem::WriteFileText(outputPath, content.str())", body)
        self.assertNotIn("std::ofstream", body)

    def test_database_cover_thumb_read_uses_stream_reader_pal(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = database.split("std::string coverPath;", 1)[1].split(
            "PreparedCoverThumb entry;", 1
        )[0]
        self.assertIn("Platform::FileSystem::StreamReader reader;", body)
        self.assertIn("reader.Open(coverPath, true)", body)
        self.assertIn("reader.Seek(0, Platform::FileSystem::StreamSeekOrigin::End)", body)
        self.assertIn("reader.Read", body)
        self.assertNotIn("std::ifstream", body)
        self.assertNotIn("Utf8ToWide", body)

    def test_database_reaper_file_list_uses_incremental_stream_reader(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = database.split("Database::PlaylistImportResult Database::importReaperFileListToPlaylist", 1)[1].split(
            "int64_t Database::createSmartPlaylist", 1
        )[0]
        self.assertIn("Platform::FileSystem::StreamReader reader;", body)
        self.assertIn("reader.Open(fileListPath, true)", body)
        self.assertIn("reader.Read", body)
        self.assertNotIn("std::ifstream", body)

    def test_database_statistics_db_size_uses_regular_file_probe_pal(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = database.split("// DB file size", 1)[1].split(
            "// Last scan timestamp", 1
        )[0]
        self.assertIn("Platform::FileSystem::ProbeRegularFile", body)
        self.assertIn("sizeAvailable", body)
        self.assertNotIn("fs::file_size", body)

    def test_database_folder_cover_mtime_uses_filesystem_pal(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = database.split(
            "// During complete import, resolve folder artwork", 1
        )[1].split("phaseCoverWorkUs.fetch_add", 1)[0]
        self.assertIn("int64_t mtime = 0", body)
        self.assertIn(
            "mtime = Platform::FileSystem::LastWriteTimeUnixSeconds(coverPath)",
            body,
        )
        self.assertIn("try {", body)
        self.assertIn("catch (...)", body)
        self.assertIn("record.folderCoverSrcMtime = mtime", body)
        self.assertNotIn("fs::last_write_time", body)

    def test_reaper_audio_record_metadata_uses_filesystem_probe_pal(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = database.split("static bool BuildReaperImportAudioRecord", 1)[1].split(
            "Database::PlaylistImportResult Database::importReaperFileListToPlaylist", 1
        )[0]
        self.assertIn("Platform::FileSystem::ProbeRegularFile", body)
        self.assertIn("Platform::FileSystem::LastWriteTimeUnixSeconds", body)
        self.assertNotIn("fs::exists", body)
        self.assertNotIn("fs::is_regular_file", body)
        self.assertNotIn("fs::file_size", body)
        self.assertNotIn("fs::last_write_time", body)

    def test_reaper_import_failure_classification_uses_path_probe_pal(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = database.split(
            "Database::PlaylistImportResult Database::importReaperFileListToPlaylist", 1
        )[1].split("bool Database::removeFromPlaylist", 1)[0]
        self.assertIn("fs::path diskPath = Utf8ToPath(path);", body)
        self.assertIn(
            "Platform::FileSystem::ProbePath(PathToUtf8(diskPath))",
            body,
        )
        self.assertIn("Platform::FileSystem::PathStatus::Present", body)
        self.assertIn("++result.unsupportedFiles;", body)
        self.assertIn("++result.missingOnDiskFiles;", body)
        self.assertIn("catch (...) {", body)
        self.assertNotIn("fs::exists(diskPath", body)

    def test_same_folder_menu_uses_database_filter_without_dead_scan_probe(self) -> None:
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = main.split(
            "if (!isMulti && ImGui::MenuItem(L(Ctx_ShowSameFolder)))", 1
        )[1].split("ImGui::Separator();", 1)[0]
        self.assertIn("state.folderFilterActive = true;", body)
        self.assertIn("RefreshSearchResults(state, true);", body)
        self.assertNotIn("Platform::FileSystem::ProbeRegularFile", body)
        self.assertNotIn("StartSameFolderScan", main)

    def test_same_folder_scan_runtime_uses_path_probe_pal(self) -> None:
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        runtime = (ROOT / "src" / "SameFolderScanRuntime.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "Platform::FileSystem::ProbePath(request.folderPathUtf8)", runtime
        )
        self.assertIn("Platform::FileSystem::PathStatus::Present", runtime)
        self.assertIn(
            "Platform::FileSystem::IsDirectory(request.folderPathUtf8)", runtime
        )
        self.assertIn("chunk.error = request.missingDirectoryErrorUtf8", runtime)
        self.assertNotIn("fs::exists", runtime)
        self.assertNotIn("fs::is_directory", runtime)
        self.assertNotIn("class SameFolderAsyncScanner", main)
        self.assertNotIn("static void StartSameFolderScan(", main)
        self.assertNotIn("static void PumpSameFolderScan(", main)

    def test_database_repair_snapshot_uses_filesystem_exists_pal(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = database.split("RepairCenterSnapshot Database::getRepairCenterSnapshot", 1)[1].split(
            "auto queryCount", 1
        )[0]
        self.assertIn("Platform::FileSystem::ProbePath(root)", body)
        self.assertIn("Platform::FileSystem::PathStatus::Missing", body)
        self.assertIn("Platform::FileSystem::PathStatus::Unreachable", body)
        self.assertNotIn("fs::exists", body)

    def test_database_health_check_uses_filesystem_exists_pal(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = database.split("HealthCheckResult Database::performHealthCheck", 1)[1].split(
            "RepairCenterSnapshot Database::getRepairCenterSnapshot", 1
        )[0]
        self.assertIn("Platform::FileSystem::ProbePath(path)", body)
        self.assertIn("Platform::FileSystem::PathStatus::Missing", body)
        self.assertNotIn("fs::exists", body)

    def test_database_effective_size_uses_path_probe_pal(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = database.split("uint64_t Database::GetDatabaseEffectiveSize", 1)[1].split(
            "Database::CacheSizeReport Database::getCacheSizeReport", 1
        )[0]
        self.assertIn("Platform::FileSystem::ProbePath(mainDbPathUtf8)", body)
        self.assertIn("Platform::FileSystem::PathStatus::Present", body)
        self.assertNotIn("fs::exists", body)

    def test_database_readonly_attach_uses_path_probe_pal(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = database.split("bool AttachReadOnlyDatabase", 1)[1].split(
            "uint64_t LogicalSqliteBytes", 1
        )[0]
        self.assertIn("Platform::FileSystem::ProbePath(utf8Path)", body)
        self.assertIn("Platform::FileSystem::PathStatus::Present", body)
        self.assertNotIn("fs::exists", body)

    def test_database_repair_file_size_probe_uses_filesystem_pal(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = database.split("uint64_t FileSizeIfExistsForRepair", 1)[1].split(
            "void AddSqlitePackageMetricsForRepair", 1
        )[0]
        self.assertIn("Platform::FileSystem::ProbeRegularFile", body)
        self.assertIn("sizeAvailable", body)
        self.assertNotIn("fs::exists", body)
        self.assertNotIn("fs::file_size", body)

    def test_database_external_waveform_fingerprint_uses_metadata_pal(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = database.split("static bool getExternalWaveformFingerprint", 1)[1].split(
            "bool Database::saveWaveformDetailByPath", 1
        )[0]
        self.assertIn("Platform::FileSystem::ProbeRegularFile", body)
        self.assertIn("Platform::FileSystem::LastWriteTimeUnixSeconds", body)
        self.assertNotIn("fs::exists", body)
        self.assertNotIn("fs::is_regular_file", body)
        self.assertNotIn("fs::file_size", body)
        self.assertNotIn("fs::last_write_time", body)

    def test_database_representative_targets_use_isolated_link_stubs(self) -> None:
        stubs = (ROOT / "tests/test_database_link_stubs.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn('#include "../src/platform/database_audio_probe.h"', stubs)
        self.assertIn("namespace Platform::DatabaseAudioProbe", stubs)
        self.assertIn(
            "bool Probe(const std::filesystem::path&, Info& out)", stubs
        )
        self.assertRegex(stubs, r"out\s*=\s*\{\};\s*return false;")
        self.assertIn('#include "../src/platform/system.h"', stubs)
        self.assertIn("namespace Platform {", stubs)
        self.assertEqual(1, stubs.count("void SetThreadBackgroundPriority()"))
        self.assertRegex(
            stubs, r"void SetThreadBackgroundPriority\(\)\s*\{\s*\}"
        )

        expected_consumers = {
            "TestStage6PlaybackLoadSeekProbe",
            "TestStage6ScanTerminalThroughputProbe",
            "TestSearchSemanticMatrix",
            "TestDbSearchFacets",
            "TestTrigramE2E",
            "TestFuzzySearchPrd",
            "TestBackfillRecoveryWorker",
            "TestScanWaveformCacheContract",
            "TestScanPauseResume",
            "TestImportPrecomputeWaveform",
            "TestCoverClickPerf",
            "TestStartupReconciliation",
            "TestProcessedFileExport",
            "TestSingleChannelExport",
            "TestSearchWorkspace",
            "TestMultiTabSync",
            "TestSubPlaylists",
            "TestSetPlaylistParent",
            "TestDatabaseFeatures",
            "TestPlaylistPerformanceContract",
            "TestPerfOptimizations",
            "TestSimilarityQueryService",
            "TestPreviewPlaylistDatabase",
        }
        behavior = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(
            encoding="utf-8"
        )
        blocks = {
            match.group(1): match.group(2)
            for match in re.finditer(
                r"add_executable\((\w+)([\s\S]*?)\n\s*\)", behavior
            )
        }
        consumers = {
            name: block
            for name, block in blocks.items()
            if "tests/test_database_link_stubs.cpp" in block
        }
        self.assertEqual(expected_consumers, set(consumers))
        for target, block in consumers.items():
            for source in (
                "tests/test_database_link_stubs.cpp",
                "src/Database.cpp",
                "src/AudioEngine.cpp",
            ):
                self.assertEqual(
                    1, block.count(source), f"{target}: unexpected {source} count"
                )
            for forbidden in (
                "src/platform/database_audio_probe.cpp",
                "src/platform/database_audio_probe_miniaudio.cpp",
                "src/platform/database_audio_probe_media_foundation.cpp",
                "src/platform/system_win32.cpp",
                "src/platform/system_macos.mm",
            ):
                self.assertNotIn(forbidden, block, f"{target}: {forbidden}")

    def test_audio_file_reader_owns_feature_and_key_decode(self) -> None:
        header = (ROOT / "src/platform/audio_file.h").read_text(encoding="utf-8")
        for token in (
            "using FrameCount = std::uint64_t", "enum class Status",
            "enum class LegacyWindowsPathPolicy", "FeatureLegacyUtf8VerbatimPrefix",
            "KeyPermissiveUtf8CanonicalPrefix", "class MonoFloatReader final",
        ):
            self.assertIn(token, header)
        for forbidden in ("ma_", "miniaudio", "AURO_AUDIO_FILE_TESTING"):
            self.assertNotIn(forbidden, header)

        future_files = (
            "src/platform/audio_file_miniaudio.cpp",
            "tests/audio_file_miniaudio_test_seam.h",
            "tests/audio_file_miniaudio_test_seam.cpp",
            "tests/audio_file_reader_scripted.h",
            "tests/audio_file_reader_scripted.cpp",
            "tests/test_feature_extractor_audio_file.cpp",
            "tests/test_key_detector_audio_file.cpp",
        )
        for relative in future_files:
            with self.subTest(missing_future_file=relative):
                self.assertTrue((ROOT / relative).is_file())
        for relative in ("src/platform/audio_file_miniaudio_test_seam.h",
                         "src/platform/audio_file_miniaudio_test_seam.cpp"):
            with self.subTest(orphan_platform_seam=relative):
                self.assertFalse((ROOT / relative).exists())
        seam = (ROOT / "tests/audio_file_miniaudio_test_seam.h").read_text(encoding="utf-8")
        for token in ("ma_result", "ma_decoder", "ma_decoder_config", "ma_uint64"):
            self.assertIn(token, seam)
        self.assertNotIn("AURO_AUDIO_MA_", seam)

        for relative in ("src/FeatureExtractor.cpp", "src/KeyDetector.cpp"):
            source = (ROOT / relative).read_text(encoding="utf-8")
            with self.subTest(unmigrated_consumer=relative):
                self.assertIn('#include "platform/audio_file.h"', source)
                self.assertNotIn("miniaudio.h", source)
                self.assertNotRegex(source, r"\bma_")
                self.assertNotIn("KDInitDecoder", source)
                self.assertNotRegex(source, r"\bUtf8ToWide(?:Permissive|Strict)?\s*\(")
                self.assertNotRegex(source, r'L"\\\\\\\\\?\\\\')

        behavior = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
        for target in ("TestAudioFileReaderBasic", "TestAudioFileReaderStatus", "TestAudioFileHeaderSelfContained",
                       "TestFeatureExtractorAudioFile", "TestKeyDetectorAudioFile"):
            with self.subTest(missing_target=target): self.assertIn(target, behavior)
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") + behavior
        macro_pattern = r"target_compile_definitions\(\s*TestAudioFileReader\s+PRIVATE\s+AURO_AUDIO_FILE_TESTING\s*\)"
        self.assertEqual((1, 1), (cmake.count("AURO_AUDIO_FILE_TESTING"),
                                 len(re.findall(macro_pattern, cmake))))
        self.assertIn("enum class MonoSampleRate", header)
        self.assertIn("Hz22050", header)
        self.assertIn("Hz44100", header)

    def test_bpm_detector_uses_audio_file_boundary(self) -> None:
        source = (ROOT / "src/BpmDetector.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "platform/audio_file.h"', source)
        self.assertIn("MonoSampleRate::Hz44100", source)
        self.assertNotIn("miniaudio.h", source)
        self.assertNotRegex(source, r"\bma_")
        self.assertNotIn('#include "platform/text_encoding.h"', source)

        findings = [
            finding for finding in boundary.scan_tree(ROOT)
            if finding.path == "src/BpmDetector.cpp"
        ]
        self.assertEqual([], findings)

        behavior = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
        self.assertIn("TestBpmDetectorAudioFile", behavior)

    def test_ai_engine_utf_helpers_preserve_legacy_permissive_contract(self) -> None:
        source = (ROOT / "src/AIEngine.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "platform/text_encoding.h"', source)

        self.assertNotIn("static std::wstring Utf8ToWide(", source)
        self.assertNotIn("static std::string WideToUtf8(", source)
        self.assertIn(
            "Platform::TextEncoding::ConvertTraditionalChineseToSimplified", source)
        for native_call in ("MultiByteToWideChar", "WideCharToMultiByte", "LCMapStringEx"):
            self.assertNotIn(native_call, source)

        findings = [
            finding for finding in boundary.scan_tree(ROOT)
            if finding.path == "src/AIEngine.cpp"
        ]
        self.assertEqual(
            0, sum(finding.rule == "audio-io-type" for finding in findings))
        self.assertEqual(
            set(),
            {finding.token for finding in findings if finding.rule == "native-include"},
        )
        self.assertNotIn("LCMapStringEx", source)
        self.assertIn(
            "Platform::TextEncoding::ConvertTraditionalChineseToSimplified", source)
        for token in (
            "Platform::Internet::kOpenTypePreconfigured",
            "Platform::Internet::kOptionConnectTimeout",
            "Platform::Internet::kDefaultHttpsPort",
            "Platform::Internet::kFlagSecure",
            "Platform::Internet::kQueryStatusCode",
        ):
            self.assertIn(token, source)
        self.assertNotIn("#include <shlobj.h>", source)
        self.assertFalse(
            {"MultiByteToWideChar", "WideCharToMultiByte"}
            & {finding.token for finding in findings}
        )

    def test_ai_engine_image_download_uses_filesystem_stream_writer(self) -> None:
        source = (ROOT / "src/AIEngine.cpp").read_text(encoding="utf-8")
        body = self._function_body(
            source,
            "static bool DownloadImageToDisk(const std::string& url, const std::string& savePath)",
        )

        self.assertIn("Platform::FileSystem::StreamWriter", body)
        self.assertIn('const std::string partPath = savePath + ".part";', body)
        self.assertIn("writer.Open(partPath, false, true)", body)
        self.assertIn("Platform::FileSystem::CopyFile(partPath, savePath, false)", body)
        self.assertIn("Platform::FileSystem::RemoveFile(partPath)", body)
        self.assertLess(body.index("writer.Close()"), body.index("Platform::FileSystem::CopyFile("))
        self.assertIn("writer.Write(std::string_view(buffer, bytesRead))", body)
        self.assertIn("writer.Close()", body)
        for native_file_call in ("CreateFileW", "WriteFile", "CloseHandle(hFile)"):
            self.assertNotIn(native_file_call, body)

    def test_ai_engine_incomplete_model_cleanup_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src/AIEngine.cpp").read_text(encoding="utf-8")
        body = source.split("bool AIEngine::DownloadModel(", 1)[1].split(
            "bool AIEngine::LoadAudioFile(", 1
        )[0]

        self.assertIn("Platform::FileSystem::RemoveFile(targetPath);", body)
        self.assertNotIn("DeleteFileW", body)

    def test_ai_engine_model_download_preserves_exclusive_native_write_semantics(self) -> None:
        source = (ROOT / "src/AIEngine.cpp").read_text(encoding="utf-8")
        body = source.split("bool AIEngine::DownloadModel(", 1)[1].split(
            "bool AIEngine::LoadAudioFile(", 1
        )[0]

        # DownloadModel keeps the historical exclusive-create and ignored
        # write-result behavior behind the system PAL boundary.
        self.assertIn("void* hFile = Platform::OpenExclusiveWriteFile(targetPath);", body)
        self.assertIn("if (!hFile)", body)
        self.assertIn("Platform::WriteFileBytes(hFile, buffer, bytesRead);", body)
        self.assertIn("Platform::CloseFileHandle(hFile);", body)
        self.assertNotIn("CreateFileW", body)
        self.assertNotIn("WriteFile(hFile", body)
        self.assertNotIn("CloseHandle(", body)

    def test_ai_engine_decoder_lifetime_uses_audio_file_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_file.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AIEngine.cpp").read_text(encoding="utf-8")
        body = source.split("bool AIEngine::LoadAudioFile(", 1)[1].split(
            "bool AIEngine::SaveAudioFile(", 1
        )[0]

        self.assertIn('#include "platform/audio_file.h"', source)
        self.assertIn("class MonoFloatReader final", header)
        self.assertIn("Platform::AudioFile::MonoFloatReader reader;", body)
        self.assertNotIn("ma_decoder_uninit(", body)

    def test_ai_engine_decoder_reads_use_audio_file_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_file.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AIEngine.cpp").read_text(encoding="utf-8")
        body = source.split("bool AIEngine::LoadAudioFile(", 1)[1].split(
            "bool AIEngine::SaveAudioFile(", 1
        )[0]

        self.assertIn("ReadResult Read(float* destination, FrameCount requestedFrames,", header)
        self.assertIn("std::vector<float> chunk(4096);", body)
        self.assertIn("const auto read = reader.Read(chunk.data(), chunk.size());", body)
        self.assertIn("read.status == Platform::AudioFile::Status::EndOfFile", body)
        self.assertIn("pcmData.insert(", body)
        self.assertNotIn("ma_decoder_get_length_in_pcm_frames(", body)
        self.assertNotIn("ma_decoder_read_pcm_frames(", body)

    def test_audio_engine_waveform_frame_counts_use_std_uint64(self) -> None:
        source = (ROOT / "src/AudioEngine.cpp").read_text(encoding="utf-8")
        helper = source.split(
            "static bool ShouldEnableProgressiveMainWaveForFile", 1
        )[1].split("WaveformData AudioEngine::BuildDisplayWaveformFromSource", 1)[0]
        build_waveform = source.split(
            "bool AudioEngine::buildWaveform(", 1
        )[1].split("// Seek-based quick waveform:", 1)[0]
        quick_waveform = source.split(
            "bool AudioEngine::buildQuickWaveform(", 1
        )[1].split("void AudioEngine::waveformWorkerLoop", 1)[0]

        self.assertIn("uint64_t totalFrames", helper)
        self.assertIn("uint64_t totalFrames", build_waveform)
        self.assertIn("uint64_t totalFrames", quick_waveform)
        self.assertNotIn("ma_uint64", helper)
        self.assertNotIn("ma_uint64", build_waveform)
        self.assertNotIn("ma_uint64", quick_waveform)

    def test_audio_engine_offline_waveform_frame_counts_use_std_uint64(self) -> None:
        source = (ROOT / "src/AudioEngine.cpp").read_text(encoding="utf-8")
        quick_waveform = source.split(
            "bool AudioEngine::generateQuickWaveformForFile(", 1
        )[1].split("bool AudioEngine::decodeFullFileToMemory(", 1)[0]
        full_decode = source.split(
            "bool AudioEngine::decodeFullFileToMemory(", 1
        )[1].split("std::vector<OutputDeviceInfo> AudioEngine::enumerateOutputDevices", 1)[0]

        self.assertIn("uint64_t totalFrames", quick_waveform)
        self.assertIn("uint64_t totalFrames", full_decode)
        self.assertNotIn("ma_uint64", quick_waveform)
        self.assertNotIn("ma_uint64", full_decode)

    def test_audio_engine_reverse_buffer_frame_counts_use_std_uint64(self) -> None:
        source = (ROOT / "src/AudioEngine.cpp").read_text(encoding="utf-8")
        reverse_load = source.split(
            "bool AudioEngine::loadReversedAudio()", 1
        )[1].split("bool AudioEngine::reloadWithReverseState()", 1)[0]
        reverse_reload = source.split(
            "bool AudioEngine::reloadWithReverseState()", 1
        )[1].split("    } else {", 1)[0]

        self.assertIn("uint64_t totalFrames", reverse_load)
        self.assertIn("uint64_t framesRead", reverse_load)
        self.assertIn("uint64_t frameCount", reverse_reload)
        self.assertNotIn("ma_uint64", reverse_load)
        self.assertNotIn("ma_uint64", reverse_reload)

    def test_audio_engine_reverse_load_decoder_initialization_uses_audio_data_source_pal(self) -> None:
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        body = source.split("bool AudioEngine::loadReversedAudio()", 1)[1].split(
            "bool AudioEngine::reloadWithReverseState()", 1
        )[0]
        self.assertIn(
            "Platform::AudioDataSource::InitializeDecoderFromFileWithFormat(", body
        )
        self.assertNotIn("ma_decoder_config_init(", body)
        self.assertNotIn("ma_decoder_init_file_w(", body)
        self.assertNotIn("ma_decoder_init_file(", body)

    def test_audio_engine_region_frame_counts_use_std_uint64(self) -> None:
        source = (ROOT / "src/AudioEngine.cpp").read_text(encoding="utf-8")
        region = source.split("bool AudioEngine::exportRegion", 1)[0].split(
            "static uint64_t ResolveExportFrameCount", 1
        )[1]

        self.assertIn("uint64_t decoderFrames", region)
        self.assertIn("uint64_t inFrame", region)
        self.assertIn("uint64_t outFrame", region)
        self.assertIn("uint64_t cursor", region)
        self.assertNotIn("ma_uint64", region)

    def test_audio_engine_main_waveform_decode_frame_counts_use_std_uint64(self) -> None:
        source = (ROOT / "src/AudioEngine.cpp").read_text(encoding="utf-8")
        decode_block = source.split(
            "            uint64_t totalFrames =", 1
        )[1].split("                        const auto cursorFrames =", 1)[0]
        decode_block = "uint64_t totalFrames =" + decode_block

        self.assertIn("uint64_t totalFrames", decode_block)
        self.assertIn("uint64_t framesRead", decode_block)
        self.assertIn("uint64_t framesRemaining", decode_block)
        self.assertNotIn("ma_uint64", decode_block)

    def test_audio_engine_processed_export_frame_counts_use_std_uint64(self) -> None:
        source = (ROOT / "src/AudioEngine.cpp").read_text(encoding="utf-8")
        processed_export = source.split(
            "bool AudioEngine::exportFileProcessed(", 1
        )[1].split("void AudioEngine::ensureSeekWorkerStarted", 1)[0]

        self.assertIn("uint64_t totalFrames", processed_export)
        self.assertIn("uint64_t framesRemaining", processed_export)
        self.assertIn("uint64_t framesRead", processed_export)
        self.assertNotIn("ma_uint64", processed_export)

    def test_audio_engine_playback_control_frame_counts_use_std_uint64(self) -> None:
        source = (ROOT / "src/AudioEngine.cpp").read_text(encoding="utf-8")
        playback = source.split(
            "void AudioEngine::play()", 1
        )[1].split("void AudioEngine::applyEffectiveVolume", 1)[0]

        self.assertIn("uint64_t inFrame", playback)
        self.assertIn("uint64_t fadePcm", playback)
        self.assertIn("uint64_t totalFrames", playback)
        self.assertNotIn("ma_uint64", playback)

    def test_audio_engine_region_export_frame_counts_use_std_uint64(self) -> None:
        source = (ROOT / "src/AudioEngine.cpp").read_text(encoding="utf-8")
        export_region = source.split(
            "bool AudioEngine::exportRegion(", 1
        )[1].split("bool AudioEngine::hasAudioAdjustments", 1)[0]

        self.assertIn("uint64_t totalFrames", export_region)
        self.assertIn("uint64_t startFrame, endFrame, framesToProcess", export_region)
        self.assertIn("uint64_t framesRemaining", export_region)
        self.assertNotIn("ma_uint64", export_region)

    def test_audio_engine_processed_export_format_probe_uses_data_source_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_data_source_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        body = source.split("bool AudioEngine::exportFileProcessed(", 1)[1].split(
            "bool AudioEngine::exportSingleChannel(", 1
        )[0]

        self.assertIn("DataFormat QueryDataFormat(BorrowedSource source) noexcept;", header)
        self.assertIn("const auto probeFormat = Platform::AudioDataSource::QueryDataFormat(", body)
        self.assertIn("Platform::AudioDataSource::BorrowDataSource(probe)", body)
        self.assertIn("fileChannels = probeFormat.channels", body)
        self.assertIn("fileSampleRate = probeFormat.sampleRate", body)
        self.assertNotIn("fileChannels = probe.outputChannels", body)
        self.assertNotIn("fileSampleRate = probe.outputSampleRate", body)
        self.assertIn("ma_data_source_get_data_format", backend)

    def test_native_float_reader_decoder_operations_use_data_source_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_data_source_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "NativeFloatReader.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "platform/audio_data_source.h"', source)
        self.assertIn("LengthInFrames QueryLength(BorrowedSource source) noexcept;", header)
        self.assertIn("ReadResult ReadPcmFrames(", header)
        self.assertIn("OperationResult SeekPcmFrame(", header)
        self.assertIn("Platform::AudioDataSource::QueryLength(", source)
        self.assertIn("Platform::AudioDataSource::ReadPcmFrames(", source)
        self.assertIn("Platform::AudioDataSource::SeekPcmFrame(", source)
        self.assertEqual(
            4,
            source.count("Platform::AudioDataSource::BorrowDataSource(decoder_)"),
        )
        self.assertIn("Platform::AudioDataSource::Decoder decoder_;", source)
        self.assertNotIn("ma_decoder decoder_", source)
        self.assertNotIn("ma_decoder_get_length_in_pcm_frames(", source)
        self.assertNotIn("ma_decoder_read_pcm_frames(", source)
        self.assertNotIn("ma_decoder_seek_to_pcm_frame(", source)

    def test_native_float_reader_decoder_initialization_uses_data_source_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_data_source_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "NativeFloatReader.cpp").read_text(encoding="utf-8")

        self.assertIn("OperationResult InitializeDecoderFromFile(", header)
        self.assertIn("void* decoderStorage, const std::string& pathUtf8", header)
        self.assertIn("OperationResult InitializeDecoderFromFile(", backend)
        self.assertIn("Platform::AudioDataSource::InitializeDecoderFromFile(", source)
        for native_call in (
            "ma_decoder_config_init(",
            "ma_decoder_init_file_w(",
            "ma_decoder_init_file(",
        ):
            self.assertNotIn(native_call, source)
        self.assertNotIn("ToWinLongPathIfNeeded", source)
        self.assertIn("ma_data_source_get_length_in_pcm_frames", backend)
        self.assertIn("ma_data_source_read_pcm_frames", backend)
        self.assertIn("ma_data_source_seek_to_pcm_frame", backend)

    def test_windows_main_particle_seed_uses_monotonic_pal(self) -> None:
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("static void Init(float W, float H)", 1)[1].split(
            "static void DrawBackground", 1
        )[0]

        self.assertIn('#include "platform/system.h"', source)
        self.assertIn(
            "srand(static_cast<unsigned>(Platform::MonotonicMilliseconds()));", body
        )
        self.assertNotIn("GetTickCount()", body)

    def test_windows_main_rapid_click_window_uses_monotonic_pal(self) -> None:
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        mark = source.split("void MarkUiLeftClickNow()", 1)[1].split(
            "bool IsRapidUiClickBurstActive()", 1
        )[0]
        active = source.split("bool IsRapidUiClickBurstActive()", 1)[1].split(
            "void SetCoverWallPriorityMode", 1
        )[0]

        self.assertIn("Platform::MonotonicMilliseconds()", mark)
        self.assertIn("Platform::MonotonicMilliseconds()", active)
        self.assertNotIn("GetTickCount64()", mark)
        self.assertNotIn("GetTickCount64()", active)
        self.assertIn("(now - lastTick) <= 320", active)

    def test_windows_main_drop_diagnostics_use_monotonic_pal(self) -> None:
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("if (msg == WM_DROPFILES)", 1)[1].split(
            "if (WM_TASKBARCREATED", 1
        )[0]

        self.assertEqual(2, body.count("lastDropTick ="))
        self.assertEqual(2, body.count("Platform::MonotonicMilliseconds()"))
        self.assertNotIn("GetTickCount64()", body)
        self.assertIn("catch (...)", body)

    def test_windows_main_drop_path_classification_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("if (msg == WM_DROPFILES)", 1)[1].split(
            "if (WM_TASKBARCREATED", 1
        )[0]

        self.assertIn("Platform::FileSystem::Exists", body)
        self.assertIn("Platform::FileSystem::IsDirectory", body)
        self.assertNotIn("GetFileAttributesW", body)
        self.assertNotIn("FILE_ATTRIBUTE_DIRECTORY", body)

    def test_windows_main_heartbeat_uses_monotonic_pal(self) -> None:
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("// Periodic heartbeat log", 1)[1].split(
            "// Fallback: poll hotkey", 1
        )[0]

        self.assertIn("ULONGLONG now = Platform::MonotonicMilliseconds();", body)
        self.assertIn("now - s_lastLoopLogTick > 5000", body)
        self.assertIn("s_lastLoopLogTick = now", body)
        self.assertNotIn("GetTickCount64()", body)

    def test_main_ui_timers_use_monotonic_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "platform/system.h"', source)
        self.assertEqual(2, source.count("Platform::MonotonicMilliseconds()"))
        self.assertNotIn("GetTickCount64()", source)

    def test_main_temp_drag_alias_lifecycle_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        cleanup = source.split("static void TrackTempDragFile", 1)[1].split(
            "static std::string CreateTempAliasForFile", 1
        )[0]
        alias = source.split("static std::string CreateTempAliasForFile", 1)[1].split(
            "static void SplitFileNameUtf8", 1
        )[0]

        self.assertIn("Platform::FileSystem::RemoveFile(g_tempDragFiles.front());", cleanup)
        self.assertNotIn("DeleteFileW", cleanup)
        self.assertIn("#ifdef CopyFile\n#undef CopyFile\n#endif", source)
        self.assertIn(
            "Platform::FileSystem::CopyFile(originalPathUtf8, dstUtf8, false);", alias
        )
        self.assertNotIn("CopyFileW", alias)

    def test_windows_main_foreground_pid_uses_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("// Frame pacing follows focus", 1)[1].split(
            "const bool audioPlaying", 1
        )[0]

        self.assertIn("uint64_t WindowProcessId(void* windowHandle);", header)
        self.assertIn("uint64_t WindowProcessId(void* windowHandle)", win32)
        self.assertIn("GetWindowThreadProcessId(static_cast<HWND>(windowHandle), &pid)", win32)
        self.assertIn("uint64_t WindowProcessId(void* /*windowHandle*/)", macos)
        self.assertIn("return 0;", macos)
        self.assertIn("fgPid == Platform::CurrentProcessId()", body)
        self.assertNotIn("GetCurrentProcessId()", body)
        self.assertIn("Platform::WindowProcessId(fgWnd)", body)
        self.assertNotIn("GetWindowThreadProcessId(fgWnd, &fgPid)", body)

    def test_windows_main_force_foreground_thread_ids_use_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("auto forceForeground = [](HWND hwnd)", 1)[1].split(
            "forceForeground(g_hwnd);", 1
        )[0]

        self.assertIn("uint32_t WindowThreadId(void* windowHandle);", header)
        self.assertIn("uint32_t WindowThreadId(void* windowHandle)", win32)
        self.assertIn("GetWindowThreadProcessId(static_cast<HWND>(windowHandle), nullptr)", win32)
        self.assertIn("uint32_t WindowThreadId(void* /*windowHandle*/)", macos)
        self.assertIn("return 0;", macos)
        self.assertIn("DWORD targetThread = static_cast<DWORD>(Platform::WindowThreadId(hwnd));", body)
        self.assertIn("foregroundThread = static_cast<DWORD>(Platform::WindowThreadId(foreground));", body)
        self.assertNotIn("GetWindowThreadProcessId(hwnd, nullptr)", body)
        self.assertNotIn("GetWindowThreadProcessId(foreground, nullptr)", body)

    def test_windows_main_single_instance_existing_pid_uses_system_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("if (ShouldUseSingleInstanceBroker() && !bootstrapHealthSuccessor)", 1)[1].split(
            "// ipcFilePath and hMutex remain in scope", 1
        )[0]

        self.assertIn(
            "const DWORD existingPid = static_cast<DWORD>(Platform::WindowProcessId(existingWnd));",
            body,
        )
        self.assertNotIn("GetWindowThreadProcessId(existingWnd, &existingPid)", body)

    def test_windows_main_foreground_actions_use_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")

        self.assertIn("bool WindowSetForeground(void* windowHandle);", header)
        self.assertIn("bool WindowSetForeground(void* windowHandle)", win32)
        self.assertIn("SetForegroundWindow(static_cast<HWND>(windowHandle)) != FALSE", win32)
        self.assertIn("bool WindowSetForeground(void* /*windowHandle*/)", macos)
        self.assertIn("return false;", macos)
        self.assertIn("Platform::WindowSetForeground(nextWnd)", source)
        self.assertIn("if (Platform::WindowSetForeground(hwnd))", source)
        self.assertIn("Platform::WindowSetForeground(g_hwnd)", source)
        self.assertIn("Platform::WindowSetForeground(existingWnd)", source)
        self.assertNotRegex(source, r"(?<!Allow)SetForegroundWindow\(")

    def test_windows_main_show_window_actions_use_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")

        self.assertIn("bool WindowShow(void* windowHandle, int command);", header)
        self.assertIn("bool WindowShow(void* windowHandle, int command)", win32)
        self.assertIn("ShowWindow(static_cast<HWND>(windowHandle), command) != FALSE", win32)
        self.assertIn("bool WindowShow(void* /*windowHandle*/, int /*command*/)", macos)
        self.assertIn("return false;", macos)
        self.assertIn("Platform::WindowShow(g_hwnd, SW_SHOW)", source)
        self.assertIn("Platform::WindowShow(g_hwnd, restoreCmd)", source)
        self.assertIn("Platform::WindowShow(hwnd, SW_SHOWMAXIMIZED)", source)
        # The isolated QA tray-exit route also uses the platform adapter.
        self.assertNotIn("ShowWindow(", source)

    def test_taskbar_activation_leaves_minimized_window_restoration_to_windows(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        activation = source.split("case WM_ACTIVATE:", 1)[1].split("case WM_CLOSE:", 1)[0]
        self.assertNotIn("Platform::WindowShow(hwnd, SW_RESTORE)", activation)

    def test_windows_main_last_error_uses_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")

        self.assertIn("uint32_t LastSystemError();", header)
        self.assertIn("uint32_t LastSystemError()", win32)
        self.assertIn("return static_cast<uint32_t>(GetLastError());", win32)
        self.assertIn("uint32_t LastSystemError()", macos)
        self.assertIn("return 0;", macos)
        self.assertIn("Platform::LastSystemError()", source)
        self.assertNotIn("GetLastError()", source)

    def test_main_last_error_reads_use_system_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("Platform::LastSystemError()", source)
        unique_drag_body = source.split("static std::string MakeUniqueDragName", 1)[1].split(
            "static void TrackTempDragFile", 1
        )[0]
        self.assertNotIn("GetLastError()", source.replace(unique_drag_body, ""))

    def test_windows_main_com_cleanup_uses_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")

        self.assertIn("void UninitThreadCom();", header)
        self.assertIn("void UninitThreadCom()", win32)
        self.assertIn("CoUninitialize();", win32)
        self.assertIn("void UninitThreadCom()", macos)
        self.assertIn("Platform::UninitThreadCom();", source)
        self.assertNotIn("CoUninitialize();", source)

    def test_windows_main_cpu_registry_read_uses_system_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("std::string BuildSysInfo()", 1)[1].split(
            "// GPU via OpenGL", 1
        )[0]

        self.assertIn("Platform::ReadRegistryString(", body)
        self.assertIn("Platform::RegistryHive::LocalMachine", body)
        self.assertIn('"HARDWARE\\\\DESCRIPTION\\\\System\\\\CentralProcessor\\\\0"', body)
        self.assertIn('"ProcessorNameString"', body)
        self.assertNotIn("RegOpenKeyExA", body)
        self.assertNotIn("RegQueryValueExA", body)
        self.assertNotIn("RegCloseKey", body)

    def test_windows_main_default_player_registry_read_uses_system_pal(self) -> None:
        source = (ROOT / "src" / "DefaultAudioPlayerAssociation.cpp").read_text(encoding="utf-8")
        body = source.split("bool IsSystemDefaultAudioPlayer()", 1)[1].split(
            "void RegisterOpenWith()", 1
        )[0]

        self.assertIn("Platform::QueryEffectiveFileAssociation(", body)
        self.assertIn("kProgId", body)
        self.assertIn('constexpr const char kProgId[] = "Auro.AudioFile";', source)
        self.assertNotIn("Explorer\\\\FileExts", body)
        self.assertNotIn("RegOpenKeyExW", body)
        self.assertNotIn("RegQueryValueExW", body)
        self.assertNotIn("RegCloseKey", body)
    def test_windows_main_registry_string_writes_use_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "DefaultAudioPlayerAssociation.cpp").read_text(encoding="utf-8")
        body = source.split("bool RegisterDefaultProgramsCapabilitiesUnder(", 1)[1].split(
            "bool RemoveDefaultPlayerUnder(", 1
        )[0]

        self.assertIn("bool WriteRegistryString(", header)
        self.assertIn("bool WriteRegistryString(", win32)
        self.assertIn("RegCreateKeyExW", win32)
        self.assertIn("RegSetValueExW", win32)
        self.assertIn("RegCloseKey", win32)
        self.assertIn("bool WriteRegistryString(", macos)
        self.assertIn("Platform::WriteRegistryString(", body)
        self.assertNotIn("AuroRegSetStr", source)
        self.assertNotIn("RegCreateKeyExW", body)
        self.assertNotIn("RegSetValueExW", body)
    def test_windows_main_non_crash_executable_queries_use_executable_path_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        register_body = (ROOT / "src" / "DefaultAudioPlayerAssociation.cpp").read_text(
            encoding="utf-8"
        ).split("bool RegisterDefaultProgramsCapabilitiesUnder(", 1)[1].split(
            "bool RemoveDefaultPlayerUnder(", 1
        )[0]
        integrity_body = source.split("// EXE integrity check", 1)[1].split(
            "// Open-source startup has no remote authorization gate.", 1
        )[0]
        for body in (register_body, integrity_body):
            self.assertIn("Platform::GetExecutablePath()", body)
            self.assertIn("Platform::ExecutablePathStatus::Success", body)
            self.assertNotIn("GetModuleFileNameW", body)
    def test_windows_main_registry_value_deletes_use_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "DefaultAudioPlayerAssociation.cpp").read_text(encoding="utf-8")
        body = source.split("bool RemoveDefaultPlayerUnder(", 1)[1].split(
            "bool IsRegisteredAsDefaultUnder(", 1
        )[0]
        clear_body = source.split("bool ClearOurProgId(", 1)[1].split(
            "std::string ClassesRoot(", 1
        )[0]

        self.assertIn("bool DeleteRegistryValue(", header)
        self.assertIn("bool DeleteRegistryValue(", win32)
        self.assertIn("RegOpenKeyExW", win32)
        self.assertIn("RegDeleteValueW", win32)
        self.assertIn("RegCloseKey", win32)
        self.assertIn("bool DeleteRegistryValue(", macos)
        self.assertIn("ClearOurProgId(", body)
        self.assertIn("Platform::DeleteRegistryValue(", clear_body)
        self.assertNotIn("RegOpenKeyExW", body)
        self.assertNotIn("RegDeleteValueW", body)
        self.assertNotIn("RegCloseKey", body)
        self.assertNotIn("RegOpenKeyExW", clear_body)
        self.assertNotIn("RegDeleteValueW", clear_body)
        self.assertNotIn("RegCloseKey", clear_body)
    def test_windows_main_shell_association_refresh_uses_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "DefaultAudioPlayerAssociation.cpp").read_text(encoding="utf-8")
        body = source.split("void SetAsDefaultPlayer()", 1)[1].split(
            "void RemoveDefaultPlayer()", 1
        )[0]

        self.assertIn("void NotifyShellAssociationsChanged();", header)
        self.assertIn("void NotifyShellAssociationsChanged()", win32)
        self.assertIn("SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);", win32)
        self.assertIn("void NotifyShellAssociationsChanged()", macos)
        self.assertIn("Platform::LaunchDefaultAppsSettings()", body)
        self.assertNotIn("SHChangeNotify", body)
        self.assertNotIn("#include <shlobj.h>", source)
        capabilities = source.split("bool RegisterDefaultProgramsCapabilitiesUnder(", 1)[1].split(
            "bool RemoveDefaultPlayerUnder(", 1
        )[0]
        self.assertIn("Platform::NotifyShellAssociationsChanged();", capabilities)
    def test_cuevi_feedback_wininet_handle_cleanup_uses_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        body = (ROOT / "src" / "CueviFeedbackClient.cpp").read_text(encoding="utf-8")

        self.assertIn("bool CloseInternetHandle(void* handle);", header)
        self.assertIn("bool CloseInternetHandle(void* handle)", win32)
        self.assertIn("InternetCloseHandle", win32)
        self.assertIn("bool CloseInternetHandle(void* handle)", macos)
        self.assertGreaterEqual(body.count("Platform::CloseInternetHandle("), 9)

    def test_cuevi_feedback_http_uses_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        body = (ROOT / "src" / "CueviFeedbackClient.cpp").read_text(encoding="utf-8")

        self.assertIn("void* OpenJsonPostRequest(void* connection, const std::wstring& path, bool secure);", header)
        self.assertIn("void* OpenJsonPostRequest(void* connection, const std::wstring& path, bool secure)", win32)
        self.assertIn("void* OpenJsonPostRequest(void*", macos)
        self.assertIn("Platform::OpenPreconfiguredInternetSession(", body)
        self.assertIn("Platform::SetInternetTimeouts(", body)
        self.assertIn("Platform::ConnectInternetHttp(", body)
        self.assertIn("Platform::OpenJsonPostRequest(", body)
        self.assertIn("Platform::SendHttpRequest(", body)
        self.assertIn("Platform::ReadInternetFile(", body)
        self.assertIn("Platform::QueryHttpStatusCode(", body)
        self.assertIn("INTERNET_FLAG_NO_AUTO_REDIRECT", win32)
        self.assertGreaterEqual(body.count("remainingTimeoutMs(deadline)"), 3)
        self.assertIn("if (!Platform::SetInternetTimeouts(session, remaining))", body)
        self.assertIn("if (!Platform::SetInternetTimeouts(request, remaining))", body)
        for native_call in (
            "InternetOpenA(",
            "InternetSetOptionA(",
            "InternetConnectA(",
            "HttpOpenRequestA(",
            "HttpSendRequestA(",
            "HttpQueryInfoA(",
        ):
            self.assertNotIn(native_call, body)
        self.assertNotIn("InternetCloseHandle(", body)

    def test_windows_main_command_line_memory_cleanup_uses_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        startup_body = self._function_body(source, "static std::string TryResolveStartupAudioFilePathFromCommandLine()")
        winmain_body = source.split("int WINAPI WinMain", 1)[1].split(
            "ApplyTheme(state.settings.theme);", 1
        )[0]

        self.assertIn("enum class ProcessArgumentStatus", header)
        self.assertIn("ProcessArgumentResult ReadProcessArgument(uint32_t index);", header)
        self.assertIn("ProcessArgumentResult ReadProcessArgument(uint32_t index)", win32)
        self.assertIn("CommandLineToArgvW", win32)
        self.assertIn("LocalFree(value);", win32)
        self.assertIn("ProcessArgumentResult ReadProcessArgument", macos)
        self.assertEqual(startup_body.count("Platform::ReadProcessArgument(1)"), 1)
        self.assertEqual(winmain_body.count("Platform::ReadProcessArgument(1)"), 2)
        self.assertNotIn("CommandLineToArgvW", startup_body)
        self.assertNotIn("CommandLineToArgvW", winmain_body)
        self.assertNotIn("Platform::FreeLocalMemory", source)

    def test_windows_main_webview_com_memory_cleanup_uses_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        login_body = source.split("static bool RunWebView2LoginScreen", 1)[1].split(
            "static void RunWebView2LoadingScreen", 1
        )[0]
        loading_body = source.split("static void RunWebView2LoadingScreen", 1)[1].split(
            "void RequestHarkenSearchExit", 1
        )[0]

        self.assertIn("void FreeComTaskMemory(void* handle);", header)
        self.assertIn("void FreeComTaskMemory(void* handle)", win32)
        self.assertIn("CoTaskMemFree(handle);", win32)
        self.assertIn("void FreeComTaskMemory(void* handle)", macos)
        self.assertEqual(login_body.count("Platform::FreeComTaskMemory("), 3)
        self.assertEqual(loading_body.count("Platform::FreeComTaskMemory("), 1)
        self.assertNotIn("CoTaskMemFree(", login_body)
        self.assertNotIn("CoTaskMemFree(", loading_body)

    def test_windows_main_diagnostic_handle_cleanup_uses_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        lifecycle = (ROOT / "src" / "DiagnosticFileLifecycle.cpp").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("AppendDiagLogLineSync", source)
        crash_body = source.split("static LONG WINAPI HarkenSearchCrashHandler", 1)[1].split(
            "namespace fs = std::filesystem;", 1
        )[0]
        diag_body = source.split("void AppendDiagLogLine(", 1)[1].split(
            "void AppendDiagLogF(", 1
        )[0]

        self.assertIn("void CloseNativeHandle(void* handle);", header)
        self.assertIn("void CloseNativeHandle(void* handle)", win32)
        self.assertIn("CloseHandle(static_cast<HANDLE>(handle));", win32)
        self.assertIn("void CloseNativeHandle(void* /*handle*/)", macos)
        self.assertEqual(1, crash_body.count("Platform::CloseNativeHandle(hFile)"))
        self.assertEqual(1, crash_body.count("Platform::CloseNativeHandle(hTxt)"))
        self.assertIn("file.appendAtGeneration(std::string_view(", diag_body)
        self.assertIn("Platform::FileSystem::StreamWriter writer;", lifecycle)
        self.assertNotIn("Platform::CloseNativeHandle(h)", diag_body)
        for body in (crash_body, diag_body, lifecycle):
            self.assertNotIn("CloseHandle(", body)

    def test_windows_main_async_diagnostic_log_uses_binary_stream_writer_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        lifecycle = (ROOT / "src" / "DiagnosticFileLifecycle.cpp").read_text(
            encoding="utf-8"
        )
        factory = source.split("HarkenSearch::DiagnosticFileLifecycle& DiagnosticFile()", 1)[1].split(
            "} // namespace", 1
        )[0]
        body = source.split("void AppendDiagLogLine(", 1)[1].split(
            "void AppendDiagLogF(", 1
        )[0]
        startup_sync = source.split("static void SyncDiagnosticFileEnabledFromSettings", 1)[1].split(
            "void AppendDiagLogLine(", 1
        )[0]

        self.assertIn("Platform::GetTempDirectory()", factory)
        self.assertIn('"HarkenSearch_diag.log"', factory)
        self.assertIn("GetHarkenSearchInstanceKeyUtf8()", factory)
        self.assertIn("static auto* file = []", factory)
        self.assertIn("return new HarkenSearch::DiagnosticFileLifecycle(", factory)
        self.assertIn("return *file;", factory)
        self.assertIn("const auto equals = line.find('=');", startup_sync)
        self.assertIn('trim(line);', startup_sync)
        self.assertIn('trim(key);', startup_sync)
        self.assertIn("file.appendAtGeneration(std::string_view(", body)
        self.assertIn("Platform::FileSystem::StreamWriter", lifecycle)
        self.assertIn("writer.Open(pathUtf8_, true, true)", lifecycle)
        self.assertIn("writer.Write(bytes.substr(", lifecycle)
        self.assertIn("writer.Close()", lifecycle)
        for native_call in ("CreateFileW(", "WriteFile(", "CloseNativeHandle("):
            self.assertNotIn(native_call, body + lifecycle)

    def test_windows_main_startup_diagnostic_mode_uses_the_last_duplicate_key(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        startup_sync = source.split("static void SyncDiagnosticFileEnabledFromSettings", 1)[1].split(
            "void AppendDiagLogLine(", 1
        )[0]
        diagnostic_branch = startup_sync.split('if (key == "diagnosticMode")', 1)[1].split(
            "if (end == std::string::npos)", 1
        )[0]

        self.assertNotIn("break;", diagnostic_branch)

    def test_windows_main_resyncs_diagnostic_mode_after_the_final_settings_path(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        startup_paths = source.split("if (s_isDevMode)", 1)[1].split(
            "io.IniFilename", 1
        )[0]

        self.assertEqual(1, startup_paths.count("SyncDiagnosticFileEnabledFromSettings();"))
        self.assertGreater(
            startup_paths.rfind("SyncDiagnosticFileEnabledFromSettings();"),
            startup_paths.rfind("g_appSettingsPathUtf8 ="),
        )

    def test_windows_main_crash_module_paths_use_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        crash_body = source.split("static LONG WINAPI HarkenSearchCrashHandler", 1)[1].split(
            "namespace fs = std::filesystem;", 1
        )[0]

        self.assertIn(
            "uint32_t GetModuleFileNameBuffer(void* moduleHandle, wchar_t* buffer, uint32_t capacity);",
            header,
        )
        self.assertIn(
            "uint32_t GetModuleFileNameBuffer(void* moduleHandle, wchar_t* buffer, uint32_t capacity)",
            win32,
        )
        self.assertIn("GetModuleFileNameW(", win32)
        self.assertIn("static_cast<HMODULE>(moduleHandle)", win32)
        self.assertIn("static_cast<DWORD>(capacity)", win32)
        self.assertIn(
            "uint32_t GetModuleFileNameBuffer(void* /*moduleHandle*/, wchar_t* /*buffer*/, uint32_t /*capacity*/)",
            macos,
        )
        self.assertEqual(2, crash_body.count("Platform::GetModuleFileNameBuffer("))
        self.assertNotIn("GetModuleFileNameW(", crash_body)

    def test_windows_main_crash_io_uses_fixed_buffer_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        crash_body = source.split("static LONG WINAPI HarkenSearchCrashHandler", 1)[1].split(
            "namespace fs = std::filesystem;", 1
        )[0]

        for declaration in (
            "uint32_t GetTempPathBuffer(wchar_t* buffer, uint32_t capacity);",
            "void* OpenCrashFile(const wchar_t* path, bool append);",
            "void* CurrentProcessHandle();",
            "bool WideToUtf8Buffer(const wchar_t* value, char* buffer, uint32_t capacity);",
        ):
            self.assertIn(declaration, header)
        for implementation in (
            "uint32_t GetTempPathBuffer(wchar_t* buffer, uint32_t capacity)",
            "void* OpenCrashFile(const wchar_t* path, bool append)",
            "void* CurrentProcessHandle()",
            "bool WideToUtf8Buffer(const wchar_t* value, char* buffer, uint32_t capacity)",
            "GetTempPathW(",
            "CreateFileW(",
            "GetCurrentProcess()",
            "WideCharToMultiByte(",
        ):
            self.assertIn(implementation, win32)
        for implementation in (
            "uint32_t GetTempPathBuffer(wchar_t* /*buffer*/, uint32_t /*capacity*/)",
            "void* OpenCrashFile(const wchar_t* /*path*/, bool /*append*/)",
            "void* CurrentProcessHandle()",
            "bool WideToUtf8Buffer(const wchar_t* /*value*/, char* /*buffer*/, uint32_t /*capacity*/)",
        ):
            self.assertIn(implementation, macos)
        self.assertEqual(4, crash_body.count("Platform::GetTempPathBuffer("))
        self.assertEqual(2, crash_body.count("Platform::OpenCrashFile("))
        self.assertEqual(2, crash_body.count("Platform::CurrentProcessHandle()"))
        self.assertEqual(2, crash_body.count("Platform::WideToUtf8Buffer("))
        self.assertIn("Platform::WriteFileBytes(hTxt", crash_body)
        for native_call in (
            "GetTempPathW(", "CreateFileW(", "GetCurrentProcess()", "WideCharToMultiByte(",
        ):
            self.assertNotIn(native_call, crash_body)

    def test_windows_main_tray_window_probes_use_system_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        exit_body = source.split("void RequestHarkenSearchExit", 1)[1].split(
            "void MaybeShowTrayMinimizeHint", 1
        )[0]
        menu_body = source.split("void ShowTrayContextMenu", 1)[1].split(
            "// Global Hotkey Helpers", 1
        )[0]

        self.assertIn("Platform::WindowIsValid(hwnd)", exit_body)
        self.assertIn("Platform::WindowIsVisible(hwnd)", menu_body)
        self.assertNotIn("IsWindow(hwnd)", exit_body)
        self.assertNotIn("IsWindowVisible(hwnd)", menu_body)

    def test_windows_main_frame_throttle_visibility_uses_system_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("// Keep QA commands alive even if the lane window is minimized/hidden.", 1)[1].split(
            "// Fallback: poll hotkey only when RegisterHotKey fails.", 1
        )[0]

        self.assertIn("!Platform::WindowIsVisible(hwnd)", body)
        self.assertIn("const bool visibleNotMinimized = Platform::WindowIsVisible(hwnd) && !IsIconic(hwnd);", body)
        self.assertIn("(int)Platform::WindowIsVisible(hwnd)", body)
        self.assertNotIn("IsWindowVisible(hwnd)", body)

    def test_windows_main_process_memory_probe_uses_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("// Periodic heartbeat log", 1)[1].split(
            "if (g_suppressImGuiDragTooltipFrames > 0)", 1
        )[0]

        self.assertIn("enum class ProcessMemoryUsageStatus", header)
        self.assertIn("struct ProcessMemoryUsageResult", header)
        self.assertIn("ProcessMemoryUsageResult QueryCurrentProcessMemoryUsage();", header)
        self.assertIn("ProcessMemoryUsageResult QueryCurrentProcessMemoryUsage()", win32)
        self.assertIn("#include <psapi.h>", win32)
        self.assertIn("GetProcessMemoryInfo(", win32)
        self.assertIn("GetCurrentProcess(),", win32)
        self.assertIn("ProcessMemoryUsageStatus::Success", win32)
        self.assertIn("ProcessMemoryUsageResult QueryCurrentProcessMemoryUsage()", macos)
        self.assertIn("return {};", macos)
        self.assertIn("Platform::QueryCurrentProcessMemoryUsage()", body)
        self.assertIn("Platform::ProcessMemoryUsageStatus::Success", body)
        self.assertIn("processMemory.privateUsageBytes / (1024.0 * 1024.0)", body)
        self.assertNotIn("#include <psapi.h>", source)
        for direct_token in (
            "PROCESS_MEMORY_COUNTERS_EX",
            "GetProcessMemoryInfo",
            "GetCurrentProcess()",
        ):
            self.assertNotIn(direct_token, body)

    def test_windows_main_mutex_cleanup_uses_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")

        self.assertIn("void CloseNativeHandle(void* handle);", header)
        self.assertIn("void CloseNativeHandle(void* handle)", win32)
        self.assertIn("CloseHandle(static_cast<HANDLE>(handle));", win32)
        self.assertIn("void CloseNativeHandle(void* /*handle*/)", macos)
        self.assertEqual(2, source.count("Platform::CloseNativeHandle(hMutex);"))
        self.assertNotIn("CloseHandle(hMutex)", source)

    def test_windows_main_manager_visibility_probes_use_system_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("void MinimizeWindowNoAnimation", 1)[1].split(
            "// Tray Icon", 1
        )[0]

        self.assertIn("Platform::WindowIsVisible(candidate)", body)
        self.assertIn("(int)Platform::WindowIsVisible(hwnd)", body)
        self.assertIn("(int)Platform::WindowIsVisible(g_hwnd)", body)
        self.assertIn("bool isVisible = Platform::WindowIsVisible(g_hwnd);", body)
        self.assertNotIn("IsWindowVisible(candidate)", body)
        self.assertNotIn("IsWindowVisible(hwnd)", body)
        self.assertNotIn("IsWindowVisible(g_hwnd)", body)

    def test_windows_main_close_visibility_probe_uses_system_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("case WM_CLOSE:", 1)[1].split(
            "case WM_SYSCOMMAND:", 1
        )[0]

        self.assertIn("!Platform::WindowIsVisible(hwnd) || IsIconic(hwnd)", body)
        self.assertNotIn("IsWindowVisible(hwnd)", body)

    def test_windows_main_frame_budget_visibility_uses_system_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("static aura::diagnostics::FrameBudgetEnvironment GetFrameBudgetEnvironment", 1)[1].split(
            "static float s_frameBudgetSavedVolume", 1
        )[0]

        self.assertIn("Platform::WindowIsVisible(g_hwnd)", body)
        self.assertNotIn("IsWindowVisible(g_hwnd)", body)

    def test_windows_main_webview_login_visibility_uses_system_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("static bool RunWebView2LoginScreen", 1)[1].split(
            "struct State", 1
        )[0]

        self.assertIn("!Platform::WindowIsVisible(hwnd)", body)
        self.assertNotIn("IsWindowVisible(hwnd)", body)

    def test_windows_main_crash_sentinel_write_uses_platform_pal(self) -> None:
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("// F-035: Crash sentinel", 1)[1].split(
            "// Settings loading may replace the path", 1
        )[0]
        compact_body = " ".join(body.split())

        self.assertIn(
            'Platform::FileSystem::WriteFileText(sentinelPath, "pid=" + '
            'std::to_string(Platform::CurrentProcessId()) + "\\n");',
            compact_body,
        )
        self.assertNotIn("GetCurrentProcessId()", body)
        self.assertNotIn("std::ofstream", body)
        self.assertIn("state.crashDetectedOnStartup = true", body)

    def test_windows_main_crash_paths_keep_crash_safe_win32_temp_apis(self) -> None:
        header = (ROOT / "src/platform/system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src/platform/system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src/platform/system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        crash_body = source.split("// ---- Write MiniDump for post-mortem debugging ----", 1)[1].split(
            "// ---- Crash-time diagnostics snapshot", 1
        )[0]
        diagnostics_body = source.split("// ---- Crash-time diagnostics snapshot", 1)[1].split(
            "// ---- Write plain-text (UTF-8) crash report", 1
        )[0]
        report_file_body = source.split("// ---- Write plain-text (UTF-8) crash report", 1)[1].split(
            "// Show a message box so user knows about the crash", 1
        )[0]
        report_body = source.split("// Show a message box so user knows about the crash", 1)[1].split(
            "if (g_prevExceptionFilter)", 1
        )[0]
        diag_log_body = source.split("void AppendDiagLogLine(", 1)[1].split(
            "void AppendDiagLogF", 1
        )[0]
        self.assertIn("wchar_t dumpPath[MAX_PATH] = {};", crash_body)
        self.assertIn("DWORD n = Platform::GetTempPathBuffer(dumpPath, MAX_PATH);", crash_body)
        self.assertIn("if (n > 0 && n < MAX_PATH)", crash_body)
        self.assertIn("wchar_t diagPath[MAX_PATH] = {};", diagnostics_body)
        self.assertIn("DWORD n = Platform::GetTempPathBuffer(diagPath, MAX_PATH);", diagnostics_body)
        self.assertIn("if (n > 0 && n < MAX_PATH)", diagnostics_body)
        self.assertIn("wchar_t txtPath[MAX_PATH] = {};", report_file_body)
        self.assertIn("DWORD n = Platform::GetTempPathBuffer(txtPath, MAX_PATH);", report_file_body)
        self.assertIn("if (n > 0 && n < MAX_PATH)", report_file_body)
        self.assertIn("void OpenCrashReportFolder(const wchar_t* path) noexcept;", header)
        self.assertIn(
            "void OpenCrashReportFolder(const wchar_t* path) noexcept", win32
        )
        crash_open_body = win32.split(
            "void OpenCrashReportFolder(const wchar_t* path) noexcept", 1
        )[1].split("void* CurrentProcessHandle()", 1)[0]
        self.assertIn("if (!path || !path[0]) return;", crash_open_body)
        self.assertEqual(
            1,
            crash_open_body.count(
                "ShellExecuteW(nullptr, L\"open\", path, nullptr, nullptr, SW_SHOWNORMAL);"
            ),
        )
        for forbidden in ("std::string", "std::wstring", "filesystem", "OpenInFileManager"):
            self.assertNotIn(forbidden, crash_open_body)
        self.assertIn(
            "void OpenCrashReportFolder(const wchar_t* /*path*/) noexcept", macos
        )
        self.assertEqual(2, report_body.count("Platform::OpenCrashReportFolder(folderPath);"))
        self.assertNotIn("ShellExecuteW", report_body)
        self.assertIn("DWORD n = Platform::GetTempPathBuffer(folderPath, MAX_PATH);", report_body)
        self.assertIn("if (n > 0 && n < MAX_PATH)", report_body)
        self.assertLess(
            report_body.index("Platform::ShowMessageBox("),
            report_body.index("Platform::OpenCrashReportFolder(folderPath);"),
        )
        for crash_block in (crash_body, diagnostics_body, report_file_body):
            self.assertNotIn("std::string", crash_block)
            self.assertNotIn("std::wstring", crash_block)
            self.assertNotIn("Utf8ToWide", crash_block)
        self.assertNotIn("Platform::GetTempDirectory()", crash_body)
        self.assertNotIn("Platform::GetTempDirectory()", diagnostics_body)
        self.assertNotIn("Platform::GetTempDirectory()", report_file_body)
        self.assertNotIn("Platform::GetTempDirectory()", report_body)
        self.assertNotIn("Platform::OpenInFileManager(", crash_body)
        self.assertNotIn("Platform::OpenInFileManager(", diagnostics_body)
        self.assertNotIn("Platform::OpenInFileManager(", report_file_body)
        self.assertNotIn("Platform::OpenInFileManager(", report_body)
        self.assertIn(
            "userDataPath = Utf8ToWide(Platform::GetTempDirectory()) + L\"Aris_WebView2\";",
            source,
        )
        self.assertIn("if (Platform::HarkenUserData::PreviewIsolationActive())", source)
        self.assertIn(r'userDataPath = Utf8ToWide(Platform::HarkenUserData::PreviewIsolationRootUtf8()) + L"\\profile\\WebView2";', source)
        lifecycle_factory = source.split(
            "HarkenSearch::DiagnosticFileLifecycle& DiagnosticFile()", 1
        )[1].split("} // namespace", 1)[0]
        self.assertIn("Platform::GetTempDirectory()", lifecycle_factory)
        self.assertIn('"HarkenSearch_diag.log"', lifecycle_factory)
        self.assertIn("DiagnosticFile().generation()", diag_log_body)

    def test_system_win32_temp_directory_rejects_zero_and_oversize_paths(self) -> None:
        source = (ROOT / "src/platform/system_win32.cpp").read_text(encoding="utf-8")
        body = source.split("std::string GetTempDirectory()", 1)[1].split(
            "std::string GetTempDragDirectory()", 1
        )[0]
        self.assertIn("DWORD n = GetTempPathBuffer(tempPath, MAX_PATH);", body)
        self.assertRegex(source, r"GetTempPathW\(\s*static_cast<DWORD>\(capacity\), buffer\)")
        self.assertIn("if (n == 0 || n >= MAX_PATH) return {};", body)

    def test_system_macos_temp_directory_guards_utf8_string(self) -> None:
        source = (ROOT / "src/platform/system_macos.mm").read_text(encoding="utf-8")
        body = source.split("std::string GetTempDirectory()", 1)[1].split(
            "std::string GetTempDragDirectory()", 1
        )[0]
        self.assertIn("const char* utf8 = base ? [base UTF8String] : nullptr;", body)
        self.assertIn("return utf8 ? std::string(utf8) : std::string();", body)
        self.assertNotIn("std::string([base UTF8String])", body)

    def test_windows_main_clean_exit_removes_crash_sentinel_through_pal(self) -> None:
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("// F-035: Remove crash sentinel on clean exit", 1)[1].split(
            'logShutdownStage("ImGuiIniSaveBegin")', 1
        )[0]

        self.assertIn("Platform::FileSystem::RemoveFile(sentinelPath);", body)
        self.assertNotIn("std::filesystem::remove", body)
        self.assertNotIn("std::error_code", body)

    def test_windows_main_crash_handler_ids_use_platform_pal(self) -> None:
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split(
            "static LONG WINAPI HarkenSearchCrashHandler", 1
        )[1].split("namespace fs = std::filesystem;", 1)[0]
        system_win32 = (ROOT / "src/platform/system_win32.cpp").read_text(
            encoding="utf-8"
        )
        thread_id_win32 = (ROOT / "src/platform/thread_id_win32.cpp").read_text(
            encoding="utf-8"
        )
        compact_body = " ".join(body.split())
        compact_system_win32 = " ".join(system_win32.split())
        compact_thread_id_win32 = " ".join(thread_id_win32.split())

        self.assertIn('#include "platform/thread_id.h"', source)
        self.assertIn(
            "MiniDumpWriteDump(static_cast<HANDLE>(Platform::CurrentProcessHandle()), "
            "static_cast<DWORD>(Platform::CurrentProcessId()), hFile,",
            compact_body,
        )
        self.assertIn(
            "mei.ThreadId = static_cast<DWORD>(Platform::CurrentThreadId());",
            compact_body,
        )
        self.assertIn(
            "static_cast<unsigned>(Platform::CurrentProcessId()), "
            "static_cast<unsigned>(Platform::CurrentThreadId())",
            compact_body,
        )
        self.assertNotIn("GetCurrentProcessId()", body)
        self.assertNotRegex(body, r"(?<!Platform::)GetCurrentThreadId\(\)")
        self.assertIn(
            "uint64_t CurrentProcessId() { "
            "return static_cast<uint64_t>(GetCurrentProcessId()); }",
            compact_system_win32,
        )
        self.assertIn(
            "uint32_t CurrentThreadId() noexcept { "
            "return static_cast<uint32_t>(GetCurrentThreadId()); }",
            compact_thread_id_win32,
        )

    def test_windows_main_focus_restore_current_thread_uses_platform_pal(self) -> None:
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("void ShowManagerWindowNow()", 1)[1].split(
            "void HandleShowManagerHotkeyToggle()", 1
        )[0]
        compact_body = " ".join(body.split())

        self.assertIn(
            "DWORD currentThread = "
            "static_cast<DWORD>(Platform::CurrentThreadId());",
            compact_body,
        )
        self.assertNotRegex(body, r"(?<!Platform::)GetCurrentThreadId\(\)")
        attach_target = "AttachThreadInput(currentThread, targetThread, TRUE);"
        attach_foreground = (
            "AttachThreadInput(currentThread, foregroundThread, TRUE);"
        )
        detach_foreground = (
            "AttachThreadInput(currentThread, foregroundThread, FALSE);"
        )
        detach_target = "AttachThreadInput(currentThread, targetThread, FALSE);"
        for token in (
            attach_target,
            attach_foreground,
            detach_foreground,
            detach_target,
        ):
            self.assertIn(token, body)
        self.assertLess(body.index(attach_target), body.index(attach_foreground))
        self.assertLess(body.index(attach_foreground), body.index(detach_foreground))
        self.assertLess(body.index(detach_foreground), body.index(detach_target))

    @staticmethod
    def _owned_ledger(findings: list[boundary.Finding], owner_slice: str = "S6-02") -> dict:
        ledger = boundary.build_ledger(findings)
        for entry in ledger["findings"]:
            entry["owner_slice"] = owner_slice
        return ledger

    @staticmethod
    def _audited_exceptions(
        findings: list[boundary.Finding],
        evidence_document: str = "docs/native-audit.md",
    ) -> dict:
        return {
            "schema": 1,
            "generated_at": "2026-07-23",
            "evidence_document": evidence_document,
            "findings": [
                {
                    "fingerprint": finding.fingerprint,
                    "path": finding.path,
                    "line": finding.line,
                    "rule": finding.rule,
                    "token": finding.token,
                    "occurrence": finding.occurrence,
                    "owner_slice": boundary._owner_slice(finding),
                    "disposition": "platform-entry",
                }
                for finding in findings
            ],
        }

    @staticmethod
    def _platform_adapter_exceptions(
        findings: list[boundary.Finding],
        evidence_document: str = (
            "docs/plans/refactor/"
            "stage-6-bootstrap-filesystem-adapter-audit.md"
        ),
    ) -> dict:
        return {
            "schema": 1,
            "generated_at": "2026-07-29",
            "evidence_document": "docs/native-audit.md",
            "findings": [
                {
                    "fingerprint": finding.fingerprint,
                    "path": finding.path,
                    "line": finding.line,
                    "rule": finding.rule,
                    "token": finding.token,
                    "occurrence": finding.occurrence,
                    "owner_slice": "BOOTSTRAP-FILESYSTEM",
                    "disposition": "platform-adapter",
                    "evidence_document": evidence_document,
                }
                for finding in findings
            ],
        }

    @staticmethod
    def _runtime_platform_adapter_exceptions(
        findings: list[boundary.Finding],
        evidence_document: str = (
            "docs/plans/refactor/"
            "stage-6-bootstrap-runtime-adapter-audit.md"
        ),
    ) -> dict:
        return {
            "schema": 1,
            "generated_at": "2026-07-29",
            "evidence_document": "docs/native-audit.md",
            "findings": [
                {
                    "fingerprint": finding.fingerprint,
                    "path": finding.path,
                    "line": finding.line,
                    "rule": finding.rule,
                    "token": finding.token,
                    "occurrence": finding.occurrence,
                    "owner_slice": "BOOTSTRAP-RUNTIME",
                    "disposition": "platform-adapter",
                    "evidence_document": evidence_document,
                }
                for finding in findings
            ],
        }

    def test_native_include_outside_platform_is_reported(self) -> None:
        findings = self._scan({"src/Feature.cpp": "#include <windows.h>\n"})
        self.assertIn("native-include", self._rules(findings))

    def test_filesystem_io_outside_platform_is_reported(self) -> None:
        findings = self._scan({"src/Feature.cpp": "std::filesystem::remove(path);\n"})
        self.assertIn("filesystem-io", self._rules(findings))

    def test_audio_ownership_type_outside_platform_is_reported(self) -> None:
        findings = self._scan({"src/Feature.cpp": "struct ma_decoder* decoder;\n"})
        self.assertIn("audio-io-type", self._rules(findings))

    def test_all_miniaudio_symbols_are_reported(self) -> None:
        findings = self._scan({"src/Feature.cpp": "ma_engine_config config = ma_engine_config_init();\nma_uint32 frames = 0;\n"})
        tokens = {finding.token for finding in findings if finding.rule == "audio-io-type"}
        self.assertTrue({"ma_engine_config", "ma_engine_config_init", "ma_uint32"}.issubset(tokens))

    def test_posix_include_and_symbol_are_reported(self) -> None:
        findings = self._scan({"src/Feature.cpp": "#include <unistd.h>\nint fd = open(path, O_RDONLY);\n"})
        rules = self._rules(findings)
        self.assertIn("native-include", rules)
        self.assertIn("native-symbol", rules)

    def test_posix_open_scanner_distinguishes_global_calls_from_qualified_members(self) -> None:
        findings = self._scan({
            "src/Feature.cpp": (
                "#include <unistd.h>\n"
                "bool Database::open(const char* path) { return true; }\n"
                "int Namespace::open(const char* path) { return 0; }\n"
                "int global_fd = ::open(path, O_RDONLY);\n"
                "int local_fd = open(path, O_RDONLY);\n"
            ),
        })
        open_findings = [
            finding for finding in findings
            if finding.rule == "native-symbol" and finding.token == "open"
        ]
        self.assertEqual(2, len(open_findings))
        self.assertEqual(
            {"int global_fd = ::open(path, O_RDONLY);", "int local_fd = open(path, O_RDONLY);"},
            {finding.source for finding in open_findings},
        )

    def test_win32_symbol_without_include_is_reported(self) -> None:
        findings = self._scan({"src/Feature.cpp": "auto handle = CreateFileW(path);\n"})
        self.assertIn("native-symbol", self._rules(findings))

    def test_extended_windows_headers_and_symbols_are_reported(self) -> None:
        findings = self._scan({
            "src/Feature.cpp": "#include <mfapi.h>\n#include <bcrypt.h>\nSleep(1);\nMessageBoxW(nullptr, text, title, 0);\nWideCharToMultiByte(0, 0, value, -1, out, size, nullptr, nullptr);\n",
        })
        tokens = {(finding.rule, finding.token.lower()) for finding in findings}
        self.assertIn(("native-include", "mfapi.h"), tokens)
        self.assertIn(("native-include", "bcrypt.h"), tokens)
        self.assertIn(("native-symbol", "sleep"), tokens)
        self.assertIn(("native-symbol", "messageboxw"), tokens)
        self.assertIn(("native-symbol", "widechartomultibyte"), tokens)

    def test_ascii_wcsicmp_symbol_is_reported(self) -> None:
        findings = self._scan({"src/Feature.cpp": "_wcsicmp(left, right);\n"})
        tokens = {finding.token.lower() for finding in findings if finding.rule == "native-symbol"}
        self.assertIn("_wcsicmp", tokens)

    def test_stream_io_and_filesystem_queries_are_reported(self) -> None:
        findings = self._scan({"src/Feature.cpp": "std::ifstream input(path);\nbool directory = std::filesystem::is_directory(path);\n"})
        tokens = {finding.token for finding in findings if finding.rule == "filesystem-io"}
        self.assertEqual({"std::ifstream", "std::filesystem::is_directory"}, tokens)

    def test_pure_filesystem_path_value_is_not_an_io_finding(self) -> None:
        findings = self._scan({"src/Feature.cpp": "std::filesystem::path path = value;\n"})
        self.assertNotIn("filesystem-io", self._rules(findings))

    def test_platform_implementation_is_not_a_business_finding(self) -> None:
        findings = self._scan({"src/platform/system_win32.cpp": "#include <windows.h>\nHANDLE handle = CreateFileW(path);\n"})
        self.assertEqual([], findings)

    def test_pure_icon_renderer_does_not_include_windows_header(self) -> None:
        source = (ROOT / "src" / "HarkenSearchIcons.cpp").read_text(encoding="utf-8")
        self.assertNotIn("#include <windows.h>", source)

    def test_icon_renderer_uses_project_gl_loader(self) -> None:
        source = (ROOT / "src" / "HarkenSearchIcons.cpp").read_text(encoding="utf-8")
        self.assertIn(
            '#include "../thirdparty/imgui/backends/imgui_impl_opengl3_loader.h"',
            source,
        )
        self.assertNotIn("#include <GL/gl.h>", source)

    def test_icon_asset_lookup_uses_platform_exe_dir(self) -> None:
        source = (ROOT / "src" / "HarkenSearchIcons.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "platform/system.h"', source)
        self.assertIn("Platform::GetExeDir()", source)
        self.assertNotIn("GetExeDirUtf8", source)
        self.assertNotIn('#include "Utils.h"', source)

    def test_auro_icons_可读探测归属文件系统能力(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        implementation = (ROOT / "src" / "platform" / "filesystem_std.cpp").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src" / "HarkenSearchIcons.cpp").read_text(encoding="utf-8")

        self.assertIn("bool CanOpenFileForRead(const std::string& pathUtf8);", header)
        self.assertIn("bool CanOpenFileForRead(const std::string& pathUtf8)", implementation)
        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::CanOpenFileForRead", source)
        self.assertNotIn("fopen", source)

    def test_ai_engine_路径状态归属文件系统能力(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AIEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("bool Exists(const std::string& pathUtf8);", header)
        self.assertIn("bool CreateDirectories(const std::string& pathUtf8);", header)
        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::Exists", source)
        self.assertIn("Platform::FileSystem::CreateDirectories", source)
        self.assertNotIn("fs::exists", source)
        self.assertNotIn("fs::create_directories", source)

    def test_小文件读写归属文件系统能力(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        feature_source = (ROOT / "src" / "FeatureManifest.cpp").read_text(encoding="utf-8")
        theme_source = (ROOT / "src" / "Theme.cpp").read_text(encoding="utf-8")

        for declaration in (
            "std::vector<uint8_t> ReadFileBytes(const std::string& pathUtf8);",
            "bool ReadFileText(const std::string& pathUtf8, std::string& outText);",
            "bool WriteFileText(const std::string& pathUtf8, const std::string& text);",
            "bool AppendFileText(const std::string& pathUtf8, const std::string& text);",
        ):
            self.assertIn(declaration, header)

        self.assertIn('#include "platform/filesystem.h"', feature_source)
        self.assertIn("Platform::FileSystem::ReadFileBytes", feature_source)
        self.assertNotIn("std::ifstream", feature_source)

        self.assertIn('#include "platform/filesystem.h"', theme_source)
        self.assertIn("Platform::FileSystem::ReadFileText", theme_source)
        self.assertIn("Platform::FileSystem::WriteFileText", theme_source)
        self.assertIn("Platform::FileSystem::AppendFileText", theme_source)
        self.assertNotIn("std::ifstream", theme_source)
        self.assertNotIn("std::ofstream", theme_source)

    def test_feature_manifest_hmac_delegates_to_native_free_pal(self) -> None:
        header_path = ROOT / "src" / "platform" / "hmac_sha256.h"
        win32_path = ROOT / "src" / "platform" / "hmac_sha256_win32.cpp"
        unsupported_path = ROOT / "src" / "platform" / "hmac_sha256_unsupported.cpp"
        self.assertTrue(header_path.is_file())
        self.assertTrue(win32_path.is_file())
        self.assertTrue(unsupported_path.is_file())

        header = header_path.read_text(encoding="utf-8")
        win32_source = win32_path.read_text(encoding="utf-8")
        unsupported = unsupported_path.read_text(encoding="utf-8")
        feature = (ROOT / "src" / "FeatureManifest.cpp").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        behavior_cmake = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(
            encoding="utf-8"
        )

        self.assertIn("namespace Platform::Crypto", header)
        for status in (
            "Unsupported", "Success", "InvalidArgument",
            "ProviderUnavailable", "HashFailed",
        ):
            self.assertIn(status, header)
        self.assertIn("std::array<uint8_t, 32> digest{}", header)
        self.assertIn("unsigned long systemError = 0", header)
        self.assertIn("HmacSha256Result ComputeHmacSha256(", header)
        for native_token in ("windows.h", "bcrypt.h", "BCRYPT", "BCrypt"):
            self.assertNotIn(native_token, header)

        for token in (
            "BCRYPT_SHA256_ALGORITHM", "BCRYPT_ALG_HANDLE_HMAC_FLAG",
            "BCryptOpenAlgorithmProvider", "BCryptCreateHash", "BCryptHashData",
            "BCryptFinishHash", "BCryptDestroyHash", "BCryptCloseAlgorithmProvider",
            "~AlgorithmHandle", "~HashHandle",
        ):
            self.assertIn(token, win32_source)
        self.assertIn("HmacSha256Status::InvalidArgument", win32_source)
        self.assertIn("HmacSha256Status::Unsupported", unsupported)
        self.assertIn("HmacSha256Status::InvalidArgument", unsupported)
        self.assertNotIn("BCrypt", unsupported)

        self.assertIn('#include "platform/hmac_sha256.h"', feature)
        self.assertIn("Platform::Crypto::ComputeHmacSha256", feature)
        self.assertIn("HmacSha256Status::Success", feature)
        call = feature.index("Platform::Crypto::ComputeHmacSha256")
        zero = feature.index("memset(key, 0, sizeof(key))", call)
        self.assertLess(call, zero)
        for native_token in ("windows.h", "bcrypt.h", "BCRYPT", "BCrypt", "hmacSha256("):
            self.assertNotIn(native_token, feature)

        self.assertIn("src/platform/hmac_sha256_win32.cpp", cmake)
        self.assertIn("src/platform/hmac_sha256_unsupported.cpp", cmake)
        self.assertIn("src/platform/hmac_sha256_win32.cpp", behavior_cmake)
        self.assertIn("src/platform/hmac_sha256_unsupported.cpp", behavior_cmake)

    def test_workflow_persistence_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "WorkflowEngine.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::ReadFileText", source)
        self.assertIn("Platform::FileSystem::WriteFileText", source)
        self.assertNotIn("std::ifstream", source)
        self.assertNotIn("std::ofstream", source)

    def test_theme_editor_export_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "ThemeEditor.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::WriteFileText(filePath, code)", source)
        self.assertNotIn("std::ofstream", source)

    def test_theme_editor_json_dialogs_use_boundary(self) -> None:
        header_path = ROOT / "src/platform/theme_json_dialogs.h"
        win32_path = ROOT / "src/platform/theme_json_dialogs_win32.cpp"
        std_path = ROOT / "src/platform/theme_json_dialogs_std.cpp"
        self.assertTrue(header_path.is_file())
        self.assertTrue(win32_path.is_file())
        self.assertTrue(std_path.is_file())
        header = header_path.read_text(encoding="utf-8")
        win32 = win32_path.read_text(encoding="utf-8")
        standard = std_path.read_text(encoding="utf-8")
        source = (ROOT / "src/ThemeEditor.cpp").read_text(encoding="utf-8")

        self.assertIn("OpenThemeJsonFileDialogUtf8(void* ownerWindow, const std::string& suggestedPathUtf8)", header)
        self.assertIn("SaveThemeJsonFileDialogUtf8(void* ownerWindow, const std::string& suggestedPathUtf8)", header)
        for token in ("commdlg.h", "OPENFILENAMEW", "GetOpenFileNameW", "GetSaveFileNameW"):
            self.assertNotIn(token, header)
            self.assertNotIn(token, source)
        self.assertIn("#include <commdlg.h>", win32)
        for token in ("GetOpenFileNameW", "GetSaveFileNameW", "Lw(Dlg_FilterThemeJson)", "Lw(Dlg_FilterJsonFiles)",
                      "OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST", "OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST"):
            self.assertIn(token, win32)
        self.assertGreaterEqual(standard.count('return "";'), 2)
        self.assertIn('#include "platform/theme_json_dialogs.h"', source)
        self.assertIn("Platform::ThemeDialogs::OpenThemeJsonFileDialogUtf8(g_hwnd,", source)
        self.assertIn("Platform::ThemeDialogs::SaveThemeJsonFileDialogUtf8(g_hwnd,", source)

    def test_user_action_timeline_export_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "UserActionLog.cpp").read_text(encoding="utf-8")
        body = source.split("bool UserActionLog::exportTimeline", 1)[1]

        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::WriteFileText(pathUtf8, content)", body)
        self.assertNotIn("fopen", body)
        self.assertNotIn("_wfopen_s", body)

    def test_database_playlist_parent_rejections_use_structured_diagnostics(self) -> None:
        source = (ROOT / "src/Database.cpp").read_text(encoding="utf-8", errors="replace")
        body = source.split("bool Database::setPlaylistParent", 1)[1].split(
            "bool Database::isFolder", 1
        )[0]

        self.assertNotIn("OutputDebugStringA", body)
        for reason in ("invalid_child", "self_loop", "child_is_folder",
                       "target_not_folder", "prepare_failed"):
            self.assertIn(f'"SetParentRejected: reason={reason}', body)

    def test_database_cleanup_missing_files_uses_platform_path_probe(self) -> None:
        source = (ROOT / "src/Database.cpp").read_text(encoding="utf-8", errors="replace")
        body = source.split("int Database::cleanupMissingFiles", 1)[1].split(
            "std::vector<std::string> Database::getAllFolders", 1
        )[0]

        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::ProbePath(filePath)", body)
        self.assertIn("Platform::FileSystem::PathStatus::Missing", body)
        for token in ("MultiByteToWideChar", "GetFileAttributesW"):
            self.assertNotIn(token, body)

    def test_audio_load_worker_uses_structured_diagnostics(self) -> None:
        source = (ROOT / "src/AudioEngine.cpp").read_text(encoding="utf-8", errors="replace")
        body = source.split("void AudioEngine::loadFileWorkerLoop", 1)[1].split(
            "bool AudioEngine::applyLoadFileResult", 1
        )[0]

        self.assertIn('DIAG_ACTION("Audio", dbuf)', body)
        self.assertIn('DIAG_ACTION("Audio", diagBuf)', body)
        self.assertIn('DIAG_SYSTEM_FMT("Audio",\n                "PlaybackLoadDecodeFailed', body)
        self.assertNotIn("OutputDebugStringA", body)

    def test_audio_waveform_worker_uses_thread_priority_pal(self) -> None:
        source = (ROOT / "src/AudioEngine.cpp").read_text(encoding="utf-8", errors="replace")
        body = source.split("void AudioEngine::waveformWorkerLoop", 1)[1].split(
            "void AudioEngine::queueWaveformRequest", 1
        )[0]

        self.assertIn('#include "platform/system.h"', source)
        self.assertIn("Platform::SetThreadBackgroundPriority();", body)
        self.assertNotIn("SetThreadPriority(", body)

    def test_windows_main_background_workers_use_thread_background_mode_pal(self) -> None:
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        main_body = source.split("static void PumpMainDetailPrewarm", 1)[1].split(
            "struct AudioDecoderPrewarmResult", 1
        )[0]
        audio_body = source.split("static AudioDecoderPrewarmResult PrewarmAudioDecoder", 1)[1].split(
            "static void EnqueueAudioDecoderPrewarmWindow", 1
        )[0]

        for body in (main_body, audio_body):
            self.assertIn("Platform::SetThreadBackgroundMode(true);", body)
            self.assertIn("Platform::SetThreadBackgroundMode(false);", body)
            self.assertNotIn("SetThreadPriority(", body)
        self.assertEqual(1, main_body.count("Platform::SetThreadBackgroundMode(true);"))
        self.assertEqual(1, main_body.count("Platform::SetThreadBackgroundMode(false);"))
        self.assertEqual(1, audio_body.count("Platform::SetThreadBackgroundMode(true);"))
        self.assertEqual(1, audio_body.count("Platform::SetThreadBackgroundMode(false);"))

    def test_audio_engine_file_names_use_text_encoding_pal(self) -> None:
        source = (ROOT / "src/AudioEngine.cpp").read_text(encoding="utf-8", errors="replace")

        self.assertIn('#include "platform/text_encoding.h"', source)
        self.assertIn("Platform::TextEncoding::Utf8ToWidePermissive(str)", source)
        self.assertEqual(4, source.count("TextEncoding::WideToUtf8Permissive(wfilename)"))
        self.assertEqual(2, source.count("TextEncoding::WideToUtf8Permissive(wext)"))
        self.assertNotIn("MultiByteToWideChar", source)
        self.assertNotIn("WideCharToMultiByte", source)

    def test_user_action_live_sink_uses_stream_writer_pal(self) -> None:
        header = (ROOT / "src" / "UserActionLog.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "UserActionLog.cpp").read_text(encoding="utf-8")

        self.assertIn("Platform::FileSystem::StreamWriter m_file;", header)
        self.assertIn("m_file.Open(pathUtf8, false, false)", source)
        self.assertIn("m_file.Write", source)
        self.assertIn("m_file.Flush", source)
        for native_io in ("FILE*", "_wfopen_s", "fopen(", "MultiByteToWideChar", "#include <windows.h>"):
            self.assertNotIn(native_io, header + source)

    def test_wwise_persistence_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "WwiseTransferPersist.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "platform/filesystem.h"', source)
        for call in ("ReadFileText", "WriteFileText", "CreateDirectories", "Exists"):
            self.assertIn(f"Platform::FileSystem::{call}", source)
        for direct_io in ("std::ifstream", "std::ofstream", "std::filesystem::create_directories", "std::filesystem::exists"):
            self.assertNotIn(direct_io, source)

    def test_coverwall_lab_settings_use_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "coverwall_lab" / "CoverWallLabState.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::ReadFileText", source)
        self.assertIn("Platform::FileSystem::WriteFileText", source)
        self.assertNotIn("std::ifstream", source)
        self.assertNotIn("std::ofstream", source)

    def test_coverwall_lab_directory_import_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "coverwall_lab" / "CoverWallLabState.cpp").read_text(encoding="utf-8")

        self.assertIn("Platform::FileSystem::Exists", source)
        self.assertIn("Platform::FileSystem::IsDirectory", source)
        self.assertIn("Platform::FileSystem::DirectoryIterator", source)
        self.assertIn("Platform::FileSystem::DirectoryReadStatus::End", source)
        self.assertIn("Platform::FileSystem::DirectoryReadStatus::Error", source)
        for direct_io in (
            "std::filesystem::exists",
            "std::filesystem::is_directory",
            "std::filesystem::recursive_directory_iterator",
        ):
            self.assertNotIn(direct_io, source)

    def test_coverwall_lab_window_is_visible_without_showwindow_symbol(self) -> None:
        source = (ROOT / "src" / "coverwall_lab" / "CoverWallLabMain.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("WS_OVERLAPPEDWINDOW | WS_VISIBLE", source)
        self.assertNotIn("ShowWindow(", source)

    def test_filesystem_pal_exposes_opaque_stream_writer(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")

        for declaration in (
            "class StreamWriter",
            "bool Open(const std::string& pathUtf8, bool append, bool binary);",
            "bool Write(std::string_view bytes);",
            "bool Flush();",
            "std::int64_t Tell() const;",
            "void Close();",
            "bool IsOpen() const;",
            "struct Impl;",
        ):
            self.assertIn(declaration, header)
        self.assertNotIn("std::ofstream", header)
        self.assertNotIn("FILE*", header)

    def test_diagnostics_normal_output_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "DiagnosticsLogger.cpp").read_text(encoding="utf-8")
        writer = source.split("void DiagnosticsLogger::writerLoop()", 1)[1].split("// Log methods", 1)[0]
        export = source.split("bool DiagnosticsLogger::exportJson(", 1)[1].split(
            "int DiagnosticsLogger::copyRecent", 1
        )[0]

        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::StreamWriter out;", writer)
        self.assertNotIn("std::ofstream", writer)
        self.assertIn("Platform::FileSystem::WriteFileText", export)
        for native_io in ("CreateFileW", "WriteFile(", "CloseHandle", "MultiByteToWideChar", "std::ofstream"):
            self.assertNotIn(native_io, export)

    def test_diagnostics_crash_export_uses_emergency_writer_pal(self) -> None:
        header_path = ROOT / "src" / "platform" / "emergency_file_writer.h"
        win32_path = ROOT / "src" / "platform" / "emergency_file_writer_win32.cpp"
        portable_path = ROOT / "src" / "platform" / "emergency_file_writer_std.cpp"
        for path in (header_path, win32_path, portable_path):
            self.assertTrue(path.is_file(), f"missing emergency writer PAL source: {path}")

        source = (ROOT / "src" / "DiagnosticsLogger.cpp").read_text(encoding="utf-8")
        header = header_path.read_text(encoding="utf-8")
        win32 = win32_path.read_text(encoding="utf-8")
        portable = portable_path.read_text(encoding="utf-8")
        crash = source.split("bool DiagnosticsLogger::exportJsonLinesCrashSafe", 1)[1]
        for declaration in (
            "class Writer",
            "bool OpenOverwriteWide(const wchar_t* path) noexcept;",
            "bool WriteAll(const void* bytes, std::size_t size) noexcept;",
            "bool Close() noexcept;",
            "std::uintptr_t native_ = 0;",
        ):
            self.assertIn(declaration, header)
        for native_token in ("windows.h", "HANDLE", "DWORD", "CreateFileW", "WriteFile", "CloseHandle"):
            self.assertNotIn(native_token, header)
        for owned_call in ("CreateFileW", "WriteFile", "CloseHandle", "written == 0"):
            self.assertIn(owned_call, win32)
        for forbidden in ("std::string", "std::vector", "std::mutex", "new ", "throw "):
            self.assertNotIn(forbidden, header + win32)
        self.assertIn("return false;", portable)
        self.assertIn('#include "platform/emergency_file_writer.h"', source)
        self.assertIn("Platform::EmergencyFile::Writer writer;", crash)
        self.assertIn("DiagLogEntry snapshot[kOverlaySize];", crash)
        self.assertIn("copyRecent(snapshot, (int)kOverlaySize)", crash)
        self.assertIn("char cat[64], det[256];", crash)
        self.assertIn("char line[512];", crash)
        self.assertIn("static_cast<size_t>(n) < sizeof(line)", crash)
        self.assertIn("(void)writer.WriteAll", crash)
        self.assertIn("(void)writer.Close();\n    return true;", crash)
        self.assertNotIn("Platform::FileSystem::StreamWriter", crash)
        for token in ("windows.h", "HANDLE", "DWORD", "CreateFileW", "WriteFile(", "CloseHandle"):
            self.assertNotIn(token, source)

    def test_diagnostics_thread_id_uses_dedicated_pal(self) -> None:
        source = (ROOT / "src" / "DiagnosticsLogger.cpp").read_text(encoding="utf-8")
        header_path = ROOT / "src" / "platform" / "thread_id.h"
        win32_path = ROOT / "src" / "platform" / "thread_id_win32.cpp"
        portable_path = ROOT / "src" / "platform" / "thread_id_std.cpp"
        for path in (header_path, win32_path, portable_path):
            self.assertTrue(path.is_file(), f"missing thread ID PAL source: {path}")
        header = header_path.read_text(encoding="utf-8")
        win32 = win32_path.read_text(encoding="utf-8")
        portable = portable_path.read_text(encoding="utf-8")
        thread_id_body = source.split("uint32_t DiagnosticsLogger::threadId()", 1)[1].split(
            "void DiagnosticsLogger::copyTrunc", 1
        )[0]
        self.assertIn('#include "platform/thread_id.h"', source)
        self.assertIn("uint32_t CurrentThreadId() noexcept;", header)
        self.assertIn("GetCurrentThreadId()", win32)
        self.assertIn("std::hash<std::thread::id>", portable)
        self.assertIn("return Platform::CurrentThreadId();", thread_id_body)
        for native_token in ("GetCurrentThreadId", "std::hash", "_WIN32"):
            self.assertNotIn(native_token, thread_id_body)

    def test_database_open_dialog_uses_dedicated_pal(self) -> None:
        pal_header_path = ROOT / "src/platform/database_file_dialog.h"
        pal_win32_path = ROOT / "src/platform/database_file_dialog_win32.cpp"
        pal_std_path = ROOT / "src/platform/database_file_dialog_std.cpp"
        for path in (pal_header_path, pal_win32_path, pal_std_path):
            self.assertTrue(path.is_file(), f"missing database dialog PAL source: {path}")

        pal_header = pal_header_path.read_text(encoding="utf-8")
        pal_win32 = pal_win32_path.read_text(encoding="utf-8")
        pal_std = pal_std_path.read_text(encoding="utf-8")
        header = (ROOT / "src" / "Utils.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "Utils.cpp").read_text(encoding="utf-8")
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")

        declaration = "std::string OpenDatabaseFileDialogUtf8(void* ownerWindow);"
        initial_declaration = (
            "std::string OpenDatabaseFileDialogWithInitialDirectoryUtf8(\n"
            "    void* ownerWindow,\n"
            "    const std::wstring& initialDirectory);"
        )
        self.assertIn(declaration, pal_header)
        self.assertIn(initial_declaration, pal_header)
        for native_token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
            self.assertNotIn(native_token, pal_header)
        self.assertIn('#include "platform/database_file_dialog.h"', pal_win32)
        self.assertIn('#include "platform/text_encoding.h"', pal_win32)
        self.assertIn("#include <commdlg.h>", pal_win32)

        pal_body = pal_win32.split(
            "std::string OpenDatabaseFileDialogUtf8(void* ownerWindow)", 1
        )[1].split(
            "std::string OpenDatabaseFileDialogWithInitialDirectoryUtf8(", 1
        )[0]
        expected_before_call = (
            "OPENFILENAMEW ofn = {};",
            'wchar_t szFileName[MAX_PATH] = L"";',
            "ofn.lStructSize = sizeof(ofn);",
            "ofn.hwndOwner = static_cast<HWND>(ownerWindow);",
            'ofn.lpstrFilter = filterBuf.c_str();',
            'Lw(Dlg_FilterAuroDb)',
            "ofn.lpstrFile = szFileName;",
            "ofn.nMaxFile = MAX_PATH;",
            "ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;",
            'ofn.lpstrDefExt = L"db";',
            'Lw(Dlg_TitleOpenDatabase)',
            'ofn.lpstrTitle = titleBuf.c_str();',
        )
        call_index = pal_body.index("GetOpenFileNameW(&ofn)")
        for token in expected_before_call:
            self.assertIn(token, pal_body)
            self.assertLess(pal_body.index(token), call_index)
        self.assertIn('if (!GetOpenFileNameW(&ofn)) return "";', pal_body)
        self.assertIn(
            "return TextEncoding::WideToUtf8(std::wstring(szFileName));", pal_body
        )
        self.assertNotIn("lpstrInitialDir", pal_body)
        self.assertNotIn("GetLastError", pal_body)
        self.assertEqual(2, pal_std.count('return "";'))
        self.assertIn("OpenDatabaseFileDialogWithInitialDirectoryUtf8", pal_std)

        for token in ("OpenDatabaseFileDialogUtf8", "commdlg.h", "OPENFILENAMEW"):
            self.assertNotIn(token, header)
            self.assertNotIn(token, source)

        legacy_call = (
            "Platform::DatabaseDialogs::OpenDatabaseFileDialogUtf8(g_hwnd)"
        )
        initial_call = (
            "Platform::DatabaseDialogs::"
            "OpenDatabaseFileDialogWithInitialDirectoryUtf8("
        )

        self.assertIn('#include "platform/database_file_dialog.h"', main)
        self.assertEqual(1, main.count(legacy_call))
        self.assertEqual(4, main.count(initial_call))
        self.assertNotIn(
            "static std::string OpenDatabaseFileDialogUtf8(HWND owner)", main
        )
        self.assertIn(
            "ofn.lpstrInitialDir = initialDirectory.c_str();", pal_win32
        )
        self.assertIn('Lw(Dlg_FilterArisDb)', pal_win32)
        self.assertIn("GetDefaultDbDirWide()", main)

        self.assertIn("static std::string SaveDatabaseFileDialogUtf8(HWND owner)", main)

    def test_database_open_dialog_pal_source_selection(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        selected_source = "${AURO_DATABASE_FILE_DIALOG_SOURCE}"
        self.assertIn(
            "set(AURO_DATABASE_FILE_DIALOG_SOURCE "
            "src/platform/database_file_dialog_win32.cpp)",
            cmake,
        )
        self.assertIn(
            "set(AURO_DATABASE_FILE_DIALOG_SOURCE "
            "src/platform/database_file_dialog_std.cpp)",
            cmake,
        )
        app_sources = cmake.split("set(APP_SOURCES", 1)[1].split(")", 1)[0]
        self.assertEqual(1, app_sources.count(selected_source))

    def test_executable_file_dialog_uses_dedicated_pal(self) -> None:
        header = (ROOT / "src/platform/executable_file_dialog.h").read_text(
            encoding="utf-8"
        )
        win32 = (ROOT / "src/platform/executable_file_dialog_win32.cpp").read_text(
            encoding="utf-8"
        )
        portable = (ROOT / "src/platform/executable_file_dialog_std.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("std::string OpenExecutableFileDialogUtf8(", header)
        for token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
            self.assertNotIn(token, header)
        self.assertIn("ofn.hwndOwner = static_cast<HWND>(ownerWindow);", win32)
        self.assertIn(
            "wcsncpy_s(szFileName, initialPath.c_str(), _TRUNCATE);", win32
        )
        self.assertIn("ofn.lpstrTitle = title.c_str();", win32)
        self.assertIn(
            "TextEncoding::WideToUtf8(std::wstring(szFileName))", win32
        )
        self.assertIn('return "";', portable)
        self.assertEqual(
            1,
            main.count(
                "Platform::ExecutableDialogs::OpenExecutableFileDialogUtf8("
            ),
        )
        self.assertNotIn(
            "static std::string OpenExecutableFileDialogUtf8(HWND owner", main
        )
        self.assertIn(
            "set(AURO_EXECUTABLE_FILE_DIALOG_SOURCE "
            "src/platform/executable_file_dialog_win32.cpp)",
            cmake,
        )
        self.assertIn(
            "set(AURO_EXECUTABLE_FILE_DIALOG_SOURCE "
            "src/platform/executable_file_dialog_std.cpp)",
            cmake,
        )
        app_sources = cmake.split("set(APP_SOURCES", 1)[1].split(")", 1)[0]
        self.assertEqual(
            1, app_sources.count("${AURO_EXECUTABLE_FILE_DIALOG_SOURCE}")
        )

    def test_avatar_image_file_dialog_uses_dedicated_pal(self) -> None:
        header = (ROOT / "src/platform/avatar_image_file_dialog.h").read_text(
            encoding="utf-8"
        )
        win32 = (ROOT / "src/platform/avatar_image_file_dialog_win32.cpp").read_text(
            encoding="utf-8"
        )
        portable = (ROOT / "src/platform/avatar_image_file_dialog_std.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("OpenAvatarImageFileDialogUtf8(void* ownerWindow);", header)
        for token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
            self.assertNotIn(token, header)
        self.assertIn("ofn.hwndOwner = static_cast<HWND>(ownerWindow);", win32)
        self.assertIn(
            "OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR", win32
        )
        self.assertIn(
            "TextEncoding::WideToUtf8(std::wstring(szFileName))", win32
        )
        self.assertIn('return "";', portable)
        self.assertEqual(
            1,
            main.count(
                "Platform::AvatarDialogs::OpenAvatarImageFileDialogUtf8(g_hwnd)"
            ),
        )
        self.assertNotIn(
            "static std::string OpenAvatarImageFileDialogUtf8(HWND owner)", main
        )
        self.assertIn(
            "set(AURO_AVATAR_IMAGE_FILE_DIALOG_SOURCE "
            "src/platform/avatar_image_file_dialog_win32.cpp)",
            cmake,
        )
        self.assertIn(
            "set(AURO_AVATAR_IMAGE_FILE_DIALOG_SOURCE "
            "src/platform/avatar_image_file_dialog_std.cpp)",
            cmake,
        )
        app_sources = cmake.split("set(APP_SOURCES", 1)[1].split(")", 1)[0]
        self.assertEqual(
            1, app_sources.count("${AURO_AVATAR_IMAGE_FILE_DIALOG_SOURCE}")
        )

    def test_reaper_file_list_dialog_uses_dedicated_pal(self) -> None:
        header = (ROOT / "src/platform/reaper_file_list_dialog.h").read_text(
            encoding="utf-8"
        )
        win32 = (ROOT / "src/platform/reaper_file_list_dialog_win32.cpp").read_text(
            encoding="utf-8"
        )
        portable = (ROOT / "src/platform/reaper_file_list_dialog_std.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("OpenReaperFileListDialogUtf8(void* ownerWindow);", header)
        for token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
            self.assertNotIn(token, header)
        self.assertIn("ofn.hwndOwner = static_cast<HWND>(ownerWindow);", win32)
        self.assertIn("OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST", win32)
        self.assertNotIn("OFN_NOCHANGEDIR", win32)
        self.assertIn('ofn.lpstrDefExt = L"ReaperFileList";', win32)
        self.assertIn(
            'Lw(Dlg_TitleImportReaperList)', win32
        )
        self.assertIn(
            "TextEncoding::WideToUtf8(std::wstring(szFileName))", win32
        )
        self.assertIn('return "";', portable)
        qualified_call = (
            "Platform::ReaperDialogs::OpenReaperFileListDialogUtf8(g_hwnd)"
        )
        self.assertEqual(1, main.count(qualified_call))
        self.assertNotIn(
            "static std::string OpenReaperFileListDialogUtf8(HWND owner)", main
        )
        menu = main.index(
            'ImGui::MenuItem(L(Main_ImportReaperList))'
        )
        guard = main.rfind("#ifdef _WIN32", 0, menu)
        call = main.index(qualified_call, menu)
        guard_end = main.index("#endif", call)
        self.assertLess(guard, menu)
        self.assertLess(menu, call)
        self.assertLess(call, guard_end)
        self.assertIn(
            "set(AURO_REAPER_FILE_LIST_DIALOG_SOURCE "
            "src/platform/reaper_file_list_dialog_win32.cpp)",
            cmake,
        )
        self.assertIn(
            "set(AURO_REAPER_FILE_LIST_DIALOG_SOURCE "
            "src/platform/reaper_file_list_dialog_std.cpp)",
            cmake,
        )
        app_sources = cmake.split("set(APP_SOURCES", 1)[1].split(")", 1)[0]
        self.assertEqual(
            1, app_sources.count("${AURO_REAPER_FILE_LIST_DIALOG_SOURCE}")
        )

    def test_thesaurus_json_dialog_uses_dedicated_pal(self) -> None:
        header = (ROOT / "src/platform/thesaurus_json_dialog.h").read_text(
            encoding="utf-8"
        )
        win32 = (ROOT / "src/platform/thesaurus_json_dialog_win32.cpp").read_text(
            encoding="utf-8"
        )
        portable = (ROOT / "src/platform/thesaurus_json_dialog_std.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("OpenThesaurusJsonFileDialogUtf8(void* ownerWindow);", header)
        for token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
            self.assertNotIn(token, header)
        self.assertIn("ofn.hwndOwner = static_cast<HWND>(ownerWindow);", win32)
        self.assertIn("OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST", win32)
        for token in (
            "OFN_NOCHANGEDIR",
            "lpstrTitle",
            "lpstrDefExt",
            "lpstrInitialDir",
        ):
            self.assertNotIn(token, win32)
        self.assertIn(
            "TextEncoding::WideToUtf8(std::wstring(szFileName))", win32
        )
        self.assertIn('return "";', portable)
        qualified_call = (
            "Platform::ThesaurusDialogs::OpenThesaurusJsonFileDialogUtf8(g_hwnd)"
        )
        self.assertEqual(1, main.count(qualified_call))
        merge_button = main.index("ImGui::Button(L(Set_ThesMerge)")
        reset_button = main.index(
            "ImGui::Button(L(Set_ThesResetDefault)", merge_button
        )
        merge_body = main[merge_button:reset_button]
        self.assertNotIn("OPENFILENAMEW", merge_body)
        self.assertNotIn("GetOpenFileNameW", merge_body)
        self.assertIn(
            "set(AURO_THESAURUS_JSON_DIALOG_SOURCE "
            "src/platform/thesaurus_json_dialog_win32.cpp)",
            cmake,
        )
        self.assertIn(
            "set(AURO_THESAURUS_JSON_DIALOG_SOURCE "
            "src/platform/thesaurus_json_dialog_std.cpp)",
            cmake,
        )
        app_sources = cmake.split("set(APP_SOURCES", 1)[1].split(")", 1)[0]
        self.assertEqual(
            1, app_sources.count("${AURO_THESAURUS_JSON_DIALOG_SOURCE}")
        )

    def test_settings_import_dialog_uses_dedicated_pal(self) -> None:
        header = (ROOT / "src/platform/settings_import_dialog.h").read_text(
            encoding="utf-8"
        )
        win32 = (ROOT / "src/platform/settings_import_dialog_win32.cpp").read_text(
            encoding="utf-8"
        )
        portable = (ROOT / "src/platform/settings_import_dialog_std.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("OpenSettingsImportFileDialogUtf8(void* ownerWindow);", header)
        for token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
            self.assertNotIn(token, header)
        self.assertIn("ofn.hwndOwner = static_cast<HWND>(ownerWindow);", win32)
        self.assertIn(
            'ofn.lpstrFilter = filterBuf.c_str();',
            win32,
        )
        self.assertIn("OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR", win32)
        for token in (
            "OFN_PATHMUSTEXIST",
            "lpstrTitle",
            "lpstrDefExt",
            "lpstrInitialDir",
        ):
            self.assertNotIn(token, win32)
        self.assertIn(
            "TextEncoding::WideToUtf8(std::wstring(filePath))", win32
        )
        self.assertIn('return "";', portable)
        qualified_call = (
            "Platform::SettingsDialogs::OpenSettingsImportFileDialogUtf8(g_hwnd)"
        )
        self.assertEqual(1, main.count(qualified_call))
        button = main.index("ImGui::Button(L(Set_ImportSettings)")
        tooltip = main.index("if (HoveredForHelp()) HarkenSearch::SetTooltip", button)
        body = main[button:tooltip]
        self.assertNotIn("OPENFILENAMEW", body)
        self.assertNotIn("GetOpenFileNameW", body)
        self.assertIn(
            "set(AURO_SETTINGS_IMPORT_DIALOG_SOURCE "
            "src/platform/settings_import_dialog_win32.cpp)",
            cmake,
        )
        self.assertIn(
            "set(AURO_SETTINGS_IMPORT_DIALOG_SOURCE "
            "src/platform/settings_import_dialog_std.cpp)",
            cmake,
        )
        app_sources = cmake.split("set(APP_SOURCES", 1)[1].split(")", 1)[0]
        self.assertEqual(
            1, app_sources.count("${AURO_SETTINGS_IMPORT_DIALOG_SOURCE}")
        )

    def test_cover_image_import_dialog_uses_dedicated_pal(self) -> None:
        header = (ROOT / "src/platform/cover_image_import_dialog.h").read_text(
            encoding="utf-8"
        )
        win32 = (
            ROOT / "src/platform/cover_image_import_dialog_win32.cpp"
        ).read_text(encoding="utf-8")
        portable = (
            ROOT / "src/platform/cover_image_import_dialog_std.cpp"
        ).read_text(encoding="utf-8")
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn(
            "OpenCoverImageImportFileDialogUtf8(void* ownerWindow);", header
        )
        for token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
            self.assertNotIn(token, header)
        self.assertIn("wchar_t szFile[260] = {0};", win32)
        self.assertIn("ofn.hwndOwner = static_cast<HWND>(ownerWindow);", win32)
        self.assertIn("ofn.lpstrFilter = filterBuf.c_str();", win32)
        for key in ("Dlg_FilterImageFiles", "Dlg_FilterJpegFiles",
                    "Dlg_FilterPngFiles", "Dlg_FilterAllFiles"):
            self.assertIn(f"Lw({key})", win32)
        self.assertIn("ofn.nFilterIndex = 1;", win32)
        self.assertIn("const std::wstring titleBuf = Lw(Dlg_TitleChooseCover);", win32)
        self.assertIn("ofn.lpstrTitle = titleBuf.c_str();", win32)
        self.assertIn("OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST", win32)
        for token in ("OFN_NOCHANGEDIR", "lpstrDefExt", "lpstrInitialDir"):
            self.assertNotIn(token, win32)
        self.assertIn("TextEncoding::WideToUtf8(std::wstring(szFile))", win32)
        self.assertIn('return "";', portable)
        qualified_call = (
            "Platform::CoverDialogs::OpenCoverImageImportFileDialogUtf8(g_hwnd)"
        )
        self.assertEqual(1, main.count(qualified_call))
        dialog_start = main.index("// COVER IMPORT DIALOG")
        dialog_end = main.index("// AI 封面生成弹窗", dialog_start)
        body = main[dialog_start:dialog_end]
        self.assertNotIn("OPENFILENAMEW", body)
        self.assertNotIn("GetOpenFileNameW", body)
        call = main.index(qualified_call, dialog_start, dialog_end)
        deferred = main.index("if (state.coverImportDeferredApply)", call, dialog_end)
        load_all = main.index("StartLoadAllInBatches(state);", deferred, dialog_end)
        decode = main.index("MetadataWriter::loadImageFile(imagePath", load_all, dialog_end)
        request = main.index("RequestOriginalAudioWriteConfirm(state)", decode, dialog_end)
        self.assertIn("AppState::OriginalAudioWritePendingKind::ImportCover", body)
        self.assertNotIn("MetadataWriter::writeAlbumArt", body)
        hide = main.index("state.showCoverImportDialog = false;", request, dialog_end)
        self.assertLess(call, deferred)
        self.assertLess(deferred, load_all)
        self.assertLess(load_all, decode)
        self.assertLess(decode, request)
        self.assertLess(request, hide)
        self.assertIn(
            "set(AURO_COVER_IMAGE_IMPORT_DIALOG_SOURCE "
            "src/platform/cover_image_import_dialog_win32.cpp)",
            cmake,
        )
        self.assertIn(
            "set(AURO_COVER_IMAGE_IMPORT_DIALOG_SOURCE "
            "src/platform/cover_image_import_dialog_std.cpp)",
            cmake,
        )
        app_sources = cmake.split("set(APP_SOURCES", 1)[1].split(")", 1)[0]
        self.assertEqual(
            1, app_sources.count("${AURO_COVER_IMAGE_IMPORT_DIALOG_SOURCE}")
        )

    def test_region_wav_export_dialog_uses_dedicated_pal(self) -> None:
        header = (ROOT / "src/platform/region_wav_export_dialog.h").read_text(
            encoding="utf-8"
        )
        win32 = (
            ROOT / "src/platform/region_wav_export_dialog_win32.cpp"
        ).read_text(encoding="utf-8")
        portable = (
            ROOT / "src/platform/region_wav_export_dialog_std.cpp"
        ).read_text(encoding="utf-8")
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("SaveRegionWavFileDialogUtf8(void* ownerWindow);", header)
        for token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
            self.assertNotIn(token, header)
        self.assertIn(
            'wchar_t szFileName[MAX_PATH] = L"region_export.wav";', win32
        )
        self.assertIn("ofn.hwndOwner = static_cast<HWND>(ownerWindow);", win32)
        self.assertIn(
            'ofn.lpstrFilter = filterBuf.c_str();',
            win32,
        )
        self.assertIn("ofn.nMaxFile = MAX_PATH;", win32)
        self.assertIn("ofn.Flags = OFN_OVERWRITEPROMPT;", win32)
        self.assertIn('ofn.lpstrDefExt = L"wav";', win32)
        self.assertIn('Lw(Dlg_TitleExportRegion)', win32)
        self.assertIn('ofn.lpstrTitle = titleBuf.c_str();', win32)
        for token in (
            "OFN_PATHMUSTEXIST",
            "OFN_FILEMUSTEXIST",
            "OFN_NOCHANGEDIR",
            "lpstrInitialDir",
        ):
            self.assertNotIn(token, win32)
        self.assertIn(
            "TextEncoding::WideToUtf8(std::wstring(szFileName))", win32
        )
        self.assertIn('return "";', portable)
        qualified_call = (
            "Platform::RegionDialogs::SaveRegionWavFileDialogUtf8(g_hwnd)"
        )
        self.assertEqual(1, main.count(qualified_call))
        dialog_start = main.index("//  MODULE 4: Loop")
        dialog_end = main.index("//  MODULE 5:", dialog_start)
        body = main[dialog_start:dialog_end]
        self.assertNotIn("OPENFILENAMEW", body)
        self.assertNotIn("GetSaveFileNameW", body)
        call = main.index(qualified_call, dialog_start, dialog_end)
        export = main.index("state.audioEngine.exportRegion(outputPath", call, dialog_end)
        error = main.index("state.audioEngine.getLastExportError()", export, dialog_end)
        message = main.index("Platform::ShowMessageBox", error, dialog_end)
        self.assertLess(call, export)
        self.assertLess(export, error)
        self.assertLess(error, message)
        self.assertIn(
            "set(AURO_REGION_WAV_EXPORT_DIALOG_SOURCE "
            "src/platform/region_wav_export_dialog_win32.cpp)",
            cmake,
        )
        self.assertIn(
            "set(AURO_REGION_WAV_EXPORT_DIALOG_SOURCE "
            "src/platform/region_wav_export_dialog_std.cpp)",
            cmake,
        )
        app_sources = cmake.split("set(APP_SOURCES", 1)[1].split(")", 1)[0]
        self.assertEqual(
            1, app_sources.count("${AURO_REGION_WAV_EXPORT_DIALOG_SOURCE}")
        )

    def test_record_removal_workflow_owns_mutation_side_effects(self) -> None:
        header = (ROOT / "src/RecordRemovalWorkflow.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/RecordRemovalWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")

        self.assertIn("FileDeletionOutcome DeleteFilesFromSystem(", header)
        for token in ("AppState", "imgui", "WindowsMain", "main.cpp"):
            self.assertNotIn(token, header)
            self.assertNotIn(token, source)
        self.assertIn("database.beginTransaction()", source)
        self.assertIn("Platform::FileSystem::RemoveFile", source)
        self.assertIn("audioEngine.unload()", source)
        self.assertNotIn("beginTransaction()", main.split(
            "static void RemoveRecordsFromDatabase(", 1
        )[1].split("// Rename file on disk", 1)[0])
        self.assertEqual(
            1,
            main.count(
                "aura::records::RemoveRecordsFromDatabase(state.database, ids)"
            ),
        )
        self.assertEqual(
            1, main.count("aura::records::DeleteFilesFromSystem(")
        )

    def test_record_rename_workflow_owns_transaction_side_effects(self) -> None:
        header = (ROOT / "src/RecordRenameWorkflow.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/RecordRenameWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        wrapper = main.split("bool RenameFileOnDisk(AppState& state,", 1)[1].split(
            "// Batch Rename Helper Functions", 1
        )[0]

        self.assertIn("RecordRenameOutcome RenameFileAndDatabase(", header)
        for token in ("AppState", "imgui", "WindowsMain", "main.cpp"):
            self.assertNotIn(token, header)
            self.assertNotIn(token, source)
        self.assertIn("Platform::FileSystem::Rename", source)
        self.assertIn("database.noteExpectedFileRename", source)
        self.assertIn("database.updateFilePath", source)
        self.assertIn("audioEngine.unload()", source)
        self.assertNotIn("std::filesystem::rename", source)
        self.assertNotIn("Platform::FileSystem::Rename", wrapper)
        self.assertNotIn("database.updateFilePath", wrapper)
        self.assertEqual(
            1, wrapper.count("aura::records::RenameFileAndDatabase(")
        )

    def test_batch_rename_policy_owns_rule_and_preview_logic(self) -> None:
        header = (ROOT / "src/BatchRenamePolicy.h").read_text(encoding="utf-8")
        source = (ROOT / "src/BatchRenamePolicy.cpp").read_text(encoding="utf-8")
        app_state = (ROOT / "src/AppState.h").read_text(encoding="utf-8")
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        preview = main.split(
            "static void GenerateBatchRenamePreview(AppState& state)", 1
        )[1].split("// Execute batch rename", 1)[0]

        self.assertIn("std::string ApplyRule(", header)
        self.assertIn("std::vector<RenamePreview> BuildPreview(", header)
        for token in (
            "AppState",
            "Database",
            "AudioEngine",
            "Localization",
            "imgui",
            "WindowsMain",
            "main.cpp",
            "platform/filesystem_",
        ):
            self.assertNotIn(token, header)
            self.assertNotIn(token, source)
        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::Exists", source)
        self.assertNotIn("std::filesystem::exists", source)
        self.assertNotIn("fs::exists", source)
        self.assertIn('#include "BatchRenamePolicy.h"', app_state)
        self.assertEqual(1, preview.count("aura::rename::BuildPreview("))
        self.assertNotIn("Platform::FileSystem::Exists", preview)
        self.assertNotIn("std::regex", preview)

    def test_batch_rename_execution_workflow_owns_status_state_machine(self) -> None:
        header = (ROOT / "src/BatchRenameExecutionWorkflow.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/BatchRenameExecutionWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        wrapper = main.split(
            "static void ExecuteBatchRename(AppState& state)", 1
        )[1].split("// Helper: check if folder name matches search", 1)[0]

        self.assertIn("std::vector<CompletedBatchRename> ExecuteEligibleRenames(", header)
        for token in (
            "AppState",
            "UndoSystem",
            "Database",
            "AudioEngine",
            "Localization",
            "imgui",
            "WindowsMain",
            "main.cpp",
            "platform/",
        ):
            self.assertNotIn(token, header)
            self.assertNotIn(token, source)
        self.assertIn("renameOperation(", source)
        self.assertIn("catch (const std::exception&)", source)
        self.assertIn("catch (...)", source)
        self.assertEqual(1, wrapper.count("aura::rename::ExecuteEligibleRenames("))
        self.assertNotIn("for (auto& preview : state.renamePreviewList)", wrapper)
        self.assertNotIn("catch (const std::exception&)", wrapper)
        self.assertIn("std::make_unique<FileRenameOp>()", wrapper)
        self.assertIn("state.undoStack.push(std::move(undoOp))", wrapper)
        self.assertIn("RefreshSearchResults(state, true)", wrapper)
        self.assertIn("state.clearSelection()", wrapper)

    def test_browser_system_folder_search_policy_is_platform_independent(self) -> None:
        header = (ROOT / "src/BrowserSystemFolderSearchPolicy.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/BrowserSystemFolderSearchPolicy.cpp").read_text(
            encoding="utf-8"
        )
        runtime_source = (ROOT / "src/BrowserSystemFolderSearchRuntime.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")

        for token in (
            "AppState",
            "ImGui",
            "Localization",
            "Database",
            "AudioEngine",
            "WindowsMain",
            "main.cpp",
            "Platform::",
            "platform/",
            "windows.h",
            "std::thread",
            "std::mutex",
            "std::atomic",
        ):
            self.assertNotIn(token, header)
            self.assertNotIn(token, source)

        self.assertNotIn("hit.isFile", source)
        self.assertNotIn("struct BrowserSystemFolderSearchHit", main)
        self.assertNotIn("struct BrowserSystemFolderSearchTreeNode", main)
        self.assertNotIn("static bool BrowserSystemFolderMatchesQuery", main)
        self.assertNotIn("static bool IsFolderAncestorOf", main)
        self.assertEqual(3, runtime_source.count("MatchesSystemFolderSearchQuery("))
        self.assertIn("aura::browser::BuildSystemFolderPathChain(", main)
        self.assertIn("aura::browser::BuildSystemFolderSearchTree(", main)
        self.assertGreaterEqual(
            main.count("aura::browser::IsSystemFolderAncestorOf("), 3
        )

    def test_browser_system_folder_search_runtime_owns_async_session(self) -> None:
        header = (ROOT / "src/BrowserSystemFolderSearchRuntime.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/BrowserSystemFolderSearchRuntime.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")

        for token in (
            "AppState",
            "ImGui",
            "Localization",
            "Database",
            "AudioEngine",
            "WindowsMain",
            "main.cpp",
            "windows.h",
            "filesystem_win32",
        ):
            self.assertNotIn(token, header)
            self.assertNotIn(token, source)

        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::DirectoryIterator", source)
        self.assertIn("std::thread(", source)
        self.assertIn("}).detach();", source)
        self.assertIn("kSystemFolderSearchPublishInterval", source)
        self.assertIn("std::memory_order_release", source)
        self.assertIn("std::memory_order_acquire", source)
        self.assertIn("std::memory_order_acq_rel", source)

        self.assertNotIn("struct BrowserSystemFolderSearchTask", main)
        self.assertNotIn("g_browserSystemFolderSearchCancel", main)
        self.assertNotIn("g_browserSystemFolderSearchTask", main)
        self.assertNotIn("std::thread([query, roots, cancel, task]", main)
        self.assertIn("BrowserSystemFolderSearchRoots(", main)
        self.assertIn("g_browserSystemFolderSearchNeedsFirstHitReveal", main)
        self.assertIn("RenderBrowserSystemFolderSearchTree", main)

    def test_utils_content_conversions_use_text_encoding_boundary(self) -> None:
        source = (ROOT / "src" / "Utils.cpp").read_text(encoding="utf-8")
        repair = source.split(
            "std::string RepairUtf8MojibakeFromAnsiIfNeeded", 1
        )[1].split("void SplitFileNameUtf8", 1)[0]
        pinyin = source.split("std::string GetPinyinInitials", 1)[1].split(
            "bool IsAudioFileSupported", 1
        )[0]

        self.assertNotIn("WideCharToMultiByte(", repair)
        self.assertNotIn("MultiByteToWideChar(", repair)
        self.assertIn(
            "Platform::TextEncoding::RepairUtf8MojibakeFromSystemAnsi(valueUtf8)",
            repair,
        )

        self.assertNotIn("WideCharToMultiByte(", pinyin)
        self.assertIn("Platform::TextEncoding::WideCharToCp936(c)", pinyin)
        self.assertIn("gbk.size() == 2", pinyin)
        self.assertIn("b1 >= 0xB0 && b1 <= 0xD7", pinyin)

    def test_search_text_index_gb2312_fallback_uses_boundary(self) -> None:
        header_path = ROOT / "src/platform/search_text_gb2312.h"
        win32_path = ROOT / "src/platform/search_text_gb2312_win32.cpp"
        std_path = ROOT / "src/platform/search_text_gb2312_std.cpp"
        self.assertTrue(header_path.is_file())
        self.assertTrue(win32_path.is_file())
        self.assertTrue(std_path.is_file())
        header = header_path.read_text(encoding="utf-8")
        win32 = win32_path.read_text(encoding="utf-8")
        standard = std_path.read_text(encoding="utf-8")
        source = (ROOT / "src/SearchTextIndex.h").read_text(encoding="utf-8")

        self.assertIn("char Gb2312InitialForCodepoint(uint32_t codepoint);", header)
        for token in ("windows.h", "BOOL", "WideCharToMultiByte"): self.assertNotIn(token, header)
        self.assertIn("#include <windows.h>", win32)
        self.assertIn("BOOL defaultCharUsed = FALSE;", win32)
        self.assertIn("WideCharToMultiByte(936, 0, &wc, 1, gbk, 2, nullptr, &defaultCharUsed)", win32)
        self.assertIn("if (len != 2 || defaultCharUsed) return '\\0';", win32)
        self.assertIn("return '\\0';", standard)
        self.assertIn('#include "platform/search_text_gb2312.h"', source)
        self.assertIn("return Platform::SearchText::Gb2312InitialForCodepoint(cp);", source)
        for token in ("#include <windows.h>", "WideCharToMultiByte", "BOOL", "#ifdef _WIN32"):
            self.assertNotIn(token, source)

    def test_backfill_file_probes_use_filesystem_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "BackfillWorkerPool.cpp").read_text(encoding="utf-8")

        self.assertIn("ReadFileBytes(const std::string& pathUtf8, size_t maxBytes)", header)
        self.assertIn("int64_t LastWriteTimeUnixSeconds(const std::string& pathUtf8);", header)
        self.assertIn("Platform::FileSystem::ReadFileBytes(pathUtf8, 50 * 1024 * 1024)", source)
        self.assertIn("Platform::FileSystem::LastWriteTimeUnixSeconds", source)
        self.assertNotIn("std::ifstream", source)
        self.assertNotIn("fs::last_write_time", source)

    def test_cover_mtime_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "CoverModule.cpp").read_text(encoding="utf-8")
        body = source.split(
            "static bool TryGetPathMtimeUnixSecondsUtf8", 1
        )[1].split("static bool StartsWith", 1)[0]

        self.assertIn("outUnixSeconds = 0", body)
        self.assertIn("if (pathUtf8.empty()) return false", body)
        self.assertIn("try {", body)
        self.assertIn("catch (...)", body)
        catch_body = body.split("catch (...)", 1)[1]
        self.assertIn("outUnixSeconds = 0", catch_body)
        self.assertIn("return false", catch_body)
        self.assertIn(
            "Platform::FileSystem::LastWriteTimeUnixSeconds(pathUtf8)", body
        )
        self.assertIn("return outUnixSeconds > 0", body)
        self.assertNotIn("fs::exists", body)
        self.assertNotIn("fs::last_write_time", body)
        self.assertNotIn("static int64_t FileTimeToUnixSeconds", source)

    def test_cover_image_file_reads_use_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "CoverModule.cpp").read_text(encoding="utf-8")
        for helper in ("LoadImageFileUtf8", "ReadFileToVectorUtf8"):
            body = source.split(f"static bool {helper}", 1)[1].split("\n}", 1)[0]
            self.assertIn("Platform::FileSystem::ReadFileBytes(filePathUtf8, 50 * 1024 * 1024)", body)
            self.assertNotIn("std::ifstream", body)
            self.assertNotIn("Utf8ToWide(filePathUtf8)", body)

        fallback = source.split("static bool ComputeEmbeddedCoverKeyForFile", 1)[1].split(
            "static bool ComputeFolderCoverKeyForFile", 1
        )[0]
        self.assertIn("Platform::FileSystem::ReadFileBytes(", fallback)
        self.assertIn("PathToUtf8(imgPath)", fallback)
        self.assertIn("32 * 1024 * 1024", fallback)
        self.assertNotIn("std::ifstream", fallback)

        visual = source.split("std::string NormalizeCoverKeyByVisualIdentity", 1)[1].split(
            "bool TryGetFolderCoverKeyFast", 1
        )[0]
        self.assertIn("Platform::FileSystem::ReadFileBytes(absolutePath, 64 * 1024 * 1024)", visual)
        self.assertNotIn("std::ifstream", visual)
        self.assertIn("Platform::FileSystem::ProbeRegularFile(absolutePath)", visual)
        self.assertNotIn("fs::exists", visual)
        self.assertNotIn("fs::is_regular_file", visual)
        self.assertNotIn("fs::file_size", visual)

    def test_cover_encoding_helpers_use_text_encoding_pal(self) -> None:
        source = (ROOT / "src" / "CoverModule.cpp").read_text(encoding="utf-8", errors="replace")

        self.assertIn('#include "platform/text_encoding.h"', source)
        self.assertIn("Platform::TextEncoding::Utf8ToWidePermissive(s)", source)
        self.assertIn("Platform::TextEncoding::WideToUtf8Permissive(ws)", source)
        self.assertNotIn("MultiByteToWideChar", source)
        self.assertNotIn("WideCharToMultiByte", source)

    def test_cover_cache_directory_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "CoverModule.cpp").read_text(encoding="utf-8")
        body = source.split("void SetCoverCacheDir", 1)[1].split(
            "void SetMemoryBudget", 1
        )[0]

        assignment = "s_coverCacheDirUtf8 = cacheDirUtf8"
        ensure = (
            "Platform::FileSystem::EnsureDirectoryExists(s_coverCacheDirUtf8);"
        )
        self.assertIn(assignment, body)
        self.assertIn("if (!s_coverCacheDirUtf8.empty())", body)
        self.assertIn("try {", body)
        self.assertIn("catch (...) { }", body)
        self.assertIn(ensure, body)
        self.assertLess(body.index(assignment), body.index(ensure))
        self.assertNotIn("MultiByteToWideChar", body)
        self.assertNotIn("fs::create_directories", body)

    def test_center_sense_data_source_hides_decoder_backend_type(self) -> None:
        header = (ROOT / "src" / "CenterSenseDataSource.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "CenterSenseDataSource.cpp").read_text(encoding="utf-8")
        audio = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        pal_header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(
            encoding="utf-8"
        )
        pal_source = (
            ROOT / "src" / "platform" / "audio_data_source_miniaudio.cpp"
        ).read_text(encoding="utf-8")

        self.assertNotRegex(header, r"\bma_")
        self.assertNotRegex(pal_header, r"\bma_")
        self.assertIn("struct SourceRef {", header)
        self.assertIn("Platform::AudioDataSource::BorrowedSource borrowed;", header)
        self.assertIn("Platform::AudioDataSource::BorrowDataSource(nullptr)", header)
        self.assertIn("bool init(SourceRef source, unsigned int sampleRateHz", header)
        for deleted_member in (
            "CenterSenseDataSource(const CenterSenseDataSource&) = delete;",
            "CenterSenseDataSource& operator=(const CenterSenseDataSource&) = delete;",
            "CenterSenseDataSource(CenterSenseDataSource&&) = delete;",
            "CenterSenseDataSource& operator=(CenterSenseDataSource&&) = delete;",
        ):
            self.assertIn(deleted_member, header)
        self.assertEqual(1, header.count("Impl* m_impl"))
        self.assertNotRegex(source, r"\bma_")
        self.assertIn("Platform::AudioDataSource::TransformSource source;", source)

        init_body = source.split("bool CenterSenseDataSource::init", 1)[1].split(
            "void CenterSenseDataSource::uninit", 1
        )[0]
        self.assertNotIn("ma_decoder", init_body)
        self.assertIn("source.borrowed.IsValid()", init_body)
        self.assertIn("next->source.Initialize(", init_body)
        self.assertEqual(
            2,
            audio.count(
                "m_centerSenseDataSource->init(Platform::AudioDataSource::BorrowDataSource(*m_decoder)"
            ),
        )
        for callback in (
            "OnRead",
            "OnSeek",
            "OnFormat",
            "OnCursor",
            "OnLength",
            "OnLooping",
        ):
            self.assertIn(callback, pal_source)

    def test_center_sense_data_source_target_links_audio_text_pal(self) -> None:
        behavior = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
        target = behavior.split(
            "add_executable(TestCenterSenseDataSource EXCLUDE_FROM_ALL", 1
        )[1].split("add_test(NAME TestCenterSenseDataSource", 1)[0]
        self.assertIn("${AURO_AUDIO_TEXT_SOURCE}", target)

    def test_audio_engine_sound_cursor_reads_use_playback_boundary(self) -> None:
        header_path = ROOT / "src" / "platform" / "audio_playback.h"
        source_path = ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp"
        self.assertTrue(header_path.is_file())
        self.assertTrue(source_path.is_file())

        header = header_path.read_text(encoding="utf-8")
        backend = source_path.read_text(encoding="utf-8")
        audio = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertNotRegex(header, r"\bma_")
        self.assertIn("class BorrowedSound final", header)
        self.assertIn("BorrowedSound BorrowSound(void* sound) noexcept;", header)
        self.assertIn("FrameIndex QueryCursor(BorrowedSound sound) noexcept;", header)
        self.assertIn("ma_sound_get_cursor_in_pcm_frames", backend)
        self.assertIn('#include "platform/audio_playback.h"', audio)
        query = "Platform::AudioPlayback::QueryCursor(m_sound->Borrow())"
        self.assertEqual(6, audio.count(query))
        self.assertNotIn("ma_sound_get_cursor_in_pcm_frames(m_sound", audio)

        cmake = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
        self.assertIn("add_executable(TestAudioPlaybackCursor", cmake)
        self.assertIn("tests/test_audio_playback_cursor.cpp", cmake)
        self.assertIn("AURO_AUDIO_PLAYBACK_TESTING", cmake)

    def test_audio_engine_remaining_sound_cursors_use_playback_boundary(self) -> None:
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        local_query = "Platform::AudioPlayback::QueryCursor(sound)"
        saved_query = "Platform::AudioPlayback::QueryCursor(savedSound)"
        self.assertEqual(2, source.count(local_query))
        self.assertEqual(1, source.count(saved_query))
        self.assertNotIn("ma_sound_get_cursor_in_pcm_frames(", source)

    def test_audio_engine_sound_initialization_uses_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("class BorrowedEngine final", header)
        self.assertIn("class BorrowedDataSource final", header)
        self.assertIn("class SoundStorage final", header)
        self.assertIn("enum class SoundInitFlags", header)
        self.assertIn("struct BackendResult", header)
        self.assertIn("BackendResult InitializeFromDataSource(", header)
        self.assertNotRegex(header, r"\bma_")
        owner_initializers = re.findall(
            r"const auto soundInitResult = (?:m_)?sound->Initialize\(", source
        )
        self.assertEqual(5, len(owner_initializers))
        self.assertNotIn("Platform::AudioPlayback::InitializeFromDataSource(", source)
        self.assertGreater(source.count("Platform::AudioPlayback::SoundInitFlags::NoSpatialization"), 0)
        self.assertGreater(source.count("Platform::AudioPlayback::SoundInitFlags::NoPitch"), 0)
        self.assertNotIn("ma_sound_init_from_data_source(", source)
        self.assertIn("ma_sound_init_from_data_source", backend)

        cmake = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
        self.assertIn("add_executable(TestAudioPlaybackInit", cmake)
        self.assertIn("tests/test_audio_playback_init.cpp", cmake)

    def test_audio_engine_sample_rate_queries_use_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("std::uint32_t QueryEngineSampleRate(BorrowedEngine engine) noexcept;", header)
        self.assertGreater(source.count("Platform::AudioPlayback::QueryEngineSampleRate("), 0)
        self.assertNotIn("ma_engine_get_sample_rate(", source)
        self.assertIn("ma_engine_get_sample_rate", backend)

    def test_audio_engine_time_queries_use_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("FrameIndex QueryEngineTimeInPcmFrames(BorrowedEngine engine) noexcept;", header)
        self.assertGreater(source.count("Platform::AudioPlayback::QueryEngineTimeInPcmFrames("), 0)
        self.assertNotIn("ma_engine_get_time_in_pcm_frames(", source)
        self.assertIn("ma_engine_get_time_in_pcm_frames", backend)

    def test_audio_engine_data_source_format_queries_use_platform_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_data_source_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        cmake = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(encoding="utf-8")

        self.assertIn("struct DataFormat", header)
        self.assertIn("DataFormat QueryDataFormat(BorrowedSource source) noexcept;", header)
        self.assertEqual(18, source.count("Platform::AudioDataSource::QueryDataFormat("))
        self.assertNotIn("ma_data_source_get_data_format(", source)
        self.assertIn("ma_data_source_get_data_format", backend)
        self.assertIn("add_executable(TestAudioDataSourceFormat", cmake)

    def test_audio_engine_decoder_cursor_queries_use_data_source_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_data_source_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("struct CursorPosition", header)
        self.assertIn("CursorPosition QueryCursor(BorrowedSource source) noexcept;", header)
        self.assertEqual(2, source.count("Platform::AudioDataSource::QueryCursor("))
        self.assertNotIn("ma_decoder_get_cursor_in_pcm_frames(", source)
        self.assertIn("ma_data_source_get_cursor_in_pcm_frames", backend)

    def test_audio_engine_decoder_length_queries_use_data_source_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_data_source_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("struct LengthInFrames", header)
        self.assertIn("LengthInFrames QueryLength(BorrowedSource source) noexcept;", header)
        self.assertEqual(13, source.count("Platform::AudioDataSource::QueryLength("))
        self.assertNotIn("ma_decoder_get_length_in_pcm_frames(", source)
        self.assertIn("ma_data_source_get_length_in_pcm_frames", backend)

    def test_audio_engine_decoder_reads_use_data_source_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_data_source_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("ReadResult ReadPcmFrames(", header)
        self.assertIn("BorrowedSource source", header)
        self.assertIn("FrameCount requestedFrames) noexcept;", header)
        self.assertEqual(14, source.count("Platform::AudioDataSource::ReadPcmFrames("))
        self.assertNotIn("ma_decoder_read_pcm_frames(", source)
        self.assertIn("ma_data_source_read_pcm_frames", backend)

    def test_audio_engine_decoder_seeks_use_data_source_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_data_source_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("OperationResult SeekPcmFrame(", header)
        self.assertIn("BorrowedSource source", header)
        self.assertIn("FrameCount frame) noexcept;", header)
        self.assertEqual(6, source.count("Platform::AudioDataSource::SeekPcmFrame("))
        self.assertNotIn("ma_decoder_seek_to_pcm_frame(", source)
        self.assertIn("ma_data_source_seek_to_pcm_frame", backend)

    def test_audio_engine_decoder_uninit_uses_data_source_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_data_source_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("class BorrowedDecoder final", header)
        self.assertIn("BorrowedSource BorrowDataSource(void* source) noexcept;", header)
        self.assertIn("BorrowedDecoder BorrowDecoder(void* decoder) noexcept;", header)
        self.assertIn("void UninitializeDecoder(BorrowedDecoder decoder) noexcept;", header)
        self.assertIn("Decoder::~Decoder() { UninitializeDecoder(*this); }", backend)
        live_source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
        self.assertEqual(37, live_source.count("Platform::AudioDataSource::UninitializeDecoder("))
        self.assertNotIn("ma_decoder_uninit(", source)
        self.assertIn("ma_decoder_uninit", backend)

    def test_audio_engine_pcm_buffers_use_data_source_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_data_source_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("InitializePcmBuffer(", header)
        self.assertIn("class PcmBuffer final", header)
        self.assertIn("OperationResult InitializePcmBuffer(\n    PcmBuffer&", header)
        self.assertIn("void* bufferStorage", header)
        self.assertIn("const float* samples", header)
        self.assertIn("void UninitializePcmBuffer(void* bufferStorage) noexcept;", header)
        self.assertEqual(2, source.count("Platform::AudioDataSource::InitializePcmBuffer("))
        self.assertGreater(source.count("Platform::AudioDataSource::UninitializePcmBuffer("), 0)
        for token in ("ma_audio_buffer_config_init(", "ma_audio_buffer_init(", "ma_audio_buffer_uninit("):
            self.assertNotIn(token, source)
            self.assertIn(token, backend)
        self.assertIn("PcmBuffer buffer", source)
        self.assertNotIn("ma_audio_buffer", source)

    def test_audio_engine_volume_uses_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("void SetEngineVolume(BorrowedEngine engine, float volume) noexcept;", header)
        self.assertGreater(source.count("Platform::AudioPlayback::SetEngineVolume("), 0)
        self.assertNotIn("ma_engine_set_volume(", source)
        self.assertIn("ma_engine_set_volume", backend)

    def test_audio_engine_borrowed_snapshots_hide_native_engine_type(self) -> None:
        audio_header = (ROOT / "src" / "AudioEngine.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        self.assertIn(
            "buildWaveform(const std::string& filePath, Platform::AudioPlayback::BorrowedEngine engine",
            audio_header,
        )
        self.assertIn(
            "buildQuickWaveform(const std::string& filePath, Platform::AudioPlayback::BorrowedEngine engine",
            audio_header,
        )
        self.assertIn(
            "static void ApplyHarnessAudioMuteIfNeeded(Platform::AudioPlayback::BorrowedEngine engine)",
            source,
        )
        self.assertIn("if (engine.IsValid() && IsHarnessAudioMuteEnabled())", source)
        self.assertIn("Platform::AudioPlayback::SetEngineVolume(engine, 0.0f)", source)
        for signature, terminator in (
            ("bool AudioEngine::buildWaveform(", "bool AudioEngine::buildQuickWaveform("),
            ("bool AudioEngine::buildQuickWaveform(", "void AudioEngine::waveformWorkerLoop()"),
            ("void AudioEngine::generateWaveform(", "void AudioEngine::requestWaveformDetail("),
            ("bool AudioEngine::buildWaveformDetailForCache(", "bool AudioEngine::DecodeFileToInterleavedF32("),
        ):
            body = source.split(signature, 1)[1].split(terminator, 1)[0]
            self.assertNotIn("ma_engine", body)

    def test_audio_engine_borrowed_sound_snapshots_hide_native_type(self) -> None:
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        for signature, terminator in (
            ("void AudioEngine::setVolume(", "void AudioEngine::setPan("),
            ("void AudioEngine::setPan(", "void AudioEngine::setPitch("),
            ("void AudioEngine::setPitch(", "void AudioEngine::setFreqShift("),
            ("void AudioEngine::setPitchMode(", "bool AudioEngine::setReverse("),
            ("bool AudioEngine::isPlaying() const", "bool AudioEngine::isPaused() const"),
            ("float AudioEngine::getPlaybackPosition() const", "float AudioEngine::getOutputLatencyInSeconds() const"),
            ("bool AudioEngine::switchOutputDevice(", "bool AudioEngine::exportSingleChannel("),
        ):
            body = source.split(signature, 1)[1].split(terminator, 1)[0]
            self.assertNotIn("ma_sound*", body)

    def test_audio_engine_exports_use_s24_writer_pal(self) -> None:
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src" / "platform" / "audio_file.h").read_text(encoding="utf-8")
        self.assertIn("class S24WavWriter final", header)
        for signature, terminator in (
            ("bool AudioEngine::exportRegion(", "bool AudioEngine::hasAudioAdjustments() const"),
            ("bool AudioEngine::exportFileProcessed(", "void AudioEngine::ensureSeekWorkerStarted()"),
        ):
            body = source.split(signature, 1)[1].split(terminator, 1)[0]
            self.assertIn("Platform::AudioFile::S24WavWriter encoder;", body)
            self.assertNotIn("ma_encoder", body)
        single_channel_body = source.split("bool AudioEngine::exportSingleChannel(", 1)[1]
        self.assertIn("Platform::AudioFile::S24WavWriter encoder;", single_channel_body)
        self.assertNotIn("ma_encoder", single_channel_body)

    def test_audio_engine_s24_conversion_uses_audio_file_pal(self) -> None:
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src" / "platform" / "audio_file.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_file_miniaudio.cpp").read_text(encoding="utf-8")

        self.assertIn("void ConvertFloatToPackedS24(", header)
        self.assertEqual(6, source.count("Platform::AudioFile::ConvertFloatToPackedS24("))
        self.assertNotIn("ma_pcm_f32_to_s24(", source)
        self.assertNotIn("ma_dither_mode_none", source)
        self.assertEqual(1, backend.count("ma_pcm_f32_to_s24("))
        self.assertIn("ma_dither_mode_none", backend)

    def test_audio_engine_resampling_uses_audio_file_pal(self) -> None:
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src" / "platform" / "audio_file.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_file_miniaudio.cpp").read_text(encoding="utf-8")

        self.assertIn("class LinearFloatResampler final", header)
        self.assertIn("struct ResampleProgress", header)
        self.assertEqual(2, source.count("Platform::AudioFile::LinearFloatResampler resampler;"))
        self.assertEqual(2, source.count("resampler.Open("))
        self.assertEqual(2, source.count("resampler.Process("))
        self.assertEqual(2, source.count("resampler.Drain("))
        self.assertEqual(2, source.count("resampler.Close()"))
        for token in (
            "ma_resampler", "ma_resampler_config", "ma_resampler_config_init",
            "ma_resampler_init", "ma_resampler_process_pcm_frames",
            "ma_resampler_uninit", "ma_format_f32", "ma_resample_algorithm_linear",
        ):
            self.assertNotIn(token, source)
        self.assertIn("ma_resampler_config_init(", backend)
        self.assertIn("ma_format_f32", backend)
        self.assertIn("ma_resample_algorithm_linear", backend)
        self.assertIn("config.linear.lpfOrder = MA_MAX_FILTER_ORDER;", backend)

    def test_audio_engine_playback_device_metrics_use_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("struct PlaybackDeviceMetrics", header)
        self.assertIn("PlaybackDeviceMetrics QueryPlaybackDeviceMetrics(BorrowedEngine engine) noexcept;", header)
        self.assertGreater(source.count("Platform::AudioPlayback::QueryPlaybackDeviceMetrics("), 0)
        self.assertEqual(0, source.count("ma_engine_get_device("))
        self.assertIn("ma_engine_get_device", backend)

    def test_audio_engine_output_device_enumeration_uses_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("struct OutputDeviceDescriptor", header)
        self.assertIn("std::vector<OutputDeviceDescriptor> EnumerateOutputDevices(BorrowedEngine engine);", header)
        self.assertGreater(source.count("Platform::AudioPlayback::EnumerateOutputDevices("), 0)
        self.assertNotIn("ma_device_get_context(", source)
        self.assertNotIn("ma_context_get_devices(", source)
        self.assertNotIn("ma_device_info", source)
        self.assertIn("ma_device_get_context", backend)
        self.assertIn("ma_context_get_devices", backend)
        self.assertIn("ma_device_info", backend)

    def test_audio_engine_initialization_uses_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("struct EngineInitOptions", header)
        self.assertIn("BackendResult InitializeEngine(BorrowedEngine engine, const EngineInitOptions& options) noexcept;", header)
        self.assertNotIn("Platform::AudioPlayback::InitializeEngine(", source)
        self.assertEqual(3, source.count("->Initialize(engineOptions)"))
        self.assertNotIn("ma_engine_config_init(", source)
        self.assertNotIn("ma_engine_init(", source)
        self.assertNotIn("ma_device_id", source)
        self.assertIn("ma_engine_config_init", backend)
        self.assertIn("ma_engine_init", backend)
        self.assertIn("ma_device_id", backend)

    def test_audio_engine_shutdown_device_stop_uses_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("BackendResult StopEngineDevice(BorrowedEngine engine) noexcept;", header)
        self.assertNotIn("Platform::AudioPlayback::StopEngineDevice(", source)
        self.assertEqual(1, source.count("->StopDevice()"))
        self.assertEqual(0, source.count("ma_engine_get_device("))
        self.assertNotIn("ma_device_stop(", source)
        self.assertIn("ma_device_stop", backend)

    def test_audio_engine_sound_uninitialization_uses_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("void UninitializeSound(BorrowedSound sound) noexcept;", header)
        self.assertIn("void Sound::Uninitialize() noexcept", backend)
        self.assertNotIn("Platform::AudioPlayback::UninitializeSound(", source)
        self.assertNotIn("ma_sound_uninit(", source)
        self.assertIn("ma_sound_uninit", backend)

    def test_audio_engine_uninitialization_uses_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("void UninitializeEngine(BorrowedEngine engine) noexcept;", header)
        self.assertNotIn("Platform::AudioPlayback::UninitializeEngine(", source)
        self.assertEqual(1, source.count("->Uninitialize()"))
        self.assertNotIn("ma_engine_uninit(", source)
        self.assertIn("ma_engine_uninit", backend)

    def test_audio_engine_owns_backend_engine_through_playback_pal(self) -> None:
        pal_header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        audio_header = (ROOT / "src" / "AudioEngine.h").read_text(encoding="utf-8")
        audio_source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("class Engine final", pal_header)
        self.assertIn("BackendResult Initialize(const EngineInitOptions& options)", pal_header)
        self.assertIn("BorrowedEngine Borrow() noexcept", pal_header)
        self.assertIn("BackendResult StopDevice() noexcept", pal_header)
        self.assertIn("void Uninitialize() noexcept", pal_header)
        self.assertNotRegex(audio_header, r"\bma_engine\b")
        self.assertNotRegex(audio_source, r"\bma_engine\b")
        self.assertIn(
            "std::unique_ptr<Platform::AudioPlayback::Engine> m_engine",
            audio_header,
        )
        self.assertIn("m_engine->Borrow()", audio_source)
        self.assertNotIn("BorrowEngine(const std::unique_ptr<Engine>& engine)", pal_header)

        shutdown = audio_source[audio_source.index("void AudioEngine::shutdown()") :]
        shutdown = shutdown[: shutdown.index("void AudioEngine::setFadeInOutEnabled")]
        move_owner = shutdown.index("std::move(m_engine)")
        self.assertLess(shutdown.index("stopSeekWorker()"), move_owner)
        self.assertLess(shutdown.index("stopWaveformWorker()"), move_owner)
        self.assertLess(shutdown.index("stopLoadFileWorker()"), move_owner)
        self.assertLess(shutdown.index("flushDeferredCleanup()"), move_owner)
        self.assertLess(shutdown.index("unload()"), move_owner)

        switch = audio_source[audio_source.index("bool AudioEngine::switchOutputDevice") :]
        self.assertGreaterEqual(
            switch.count("std::make_unique<Platform::AudioPlayback::Engine>()"),
            2,
        )
        self.assertIn("engineOptions.playbackDeviceIdBlob.clear()", switch)
        self.assertIn('m_outputDeviceName = "Default"', switch)

    def test_audio_engine_owns_decoder_lifetimes_through_data_source_pal(self) -> None:
        pal_header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(encoding="utf-8")
        audio_header = (ROOT / "src" / "AudioEngine.h").read_text(encoding="utf-8")
        audio_source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("class Decoder final", pal_header)
        self.assertIn("Decoder(Decoder&&) noexcept", pal_header)
        self.assertIn("~Decoder()", pal_header)
        self.assertGreaterEqual(pal_header.count("void* NativeHandle() noexcept;"), 2)
        self.assertNotRegex(audio_header, r"\bma_decoder\b")
        self.assertNotRegex(audio_source, r"\bma_decoder\b")
        owner_type = "std::unique_ptr<Platform::AudioDataSource::Decoder>"
        self.assertGreaterEqual(audio_header.count(owner_type), 3)
        self.assertIn(f"{owner_type} m_decoder", audio_header)
        self.assertIn(
            f"bool applyLoadFileResult({owner_type} decoder",
            audio_header,
        )
        self.assertIn("auto oldDecoder = std::move(m_decoder)", audio_source)
        self.assertIn("result.decoder = std::move(dec)", audio_source)
        self.assertIn("applyLoadFileResult(std::move(r.decoder)", audio_source)
        self.assertNotIn("new ma_decoder", audio_source)
        self.assertNotIn("delete oldDecoder", audio_source)
        self.assertNotIn("delete m_decoder", audio_source)

    def test_audio_engine_owns_sound_lifetimes_through_playback_pal(self) -> None:
        pal_header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        audio_header = (ROOT / "src" / "AudioEngine.h").read_text(encoding="utf-8")
        audio_source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("class Sound final", pal_header)
        self.assertIn("Sound(Sound&&) noexcept", pal_header)
        self.assertIn("BackendResult Initialize(", pal_header)
        self.assertIn("BorrowedSound Borrow() noexcept", pal_header)
        self.assertIn("void Uninitialize() noexcept", pal_header)
        self.assertNotRegex(audio_header, r"\bma_sound\b")
        self.assertNotRegex(audio_source, r"\bma_sound\b")
        owner_type = "std::unique_ptr<Platform::AudioPlayback::Sound>"
        self.assertGreaterEqual(audio_header.count(owner_type), 2)
        self.assertIn(f"{owner_type} m_sound", audio_header)
        self.assertIn("auto oldSound = std::move(m_sound)", audio_source)
        self.assertIn("m_sound->Borrow()", audio_source)
        self.assertIn("sound->Initialize(", audio_source)
        self.assertNotIn("new ma_sound", audio_source)
        self.assertNotIn("delete oldSound", audio_source)
        self.assertNotIn("delete m_sound", audio_source)

    def test_audio_engine_playing_state_uses_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("bool IsPlaying(BorrowedSound sound) noexcept;", header)
        self.assertGreater(source.count("Platform::AudioPlayback::IsPlaying("), 0)
        self.assertNotIn("ma_sound_is_playing(", source)
        self.assertIn("ma_sound_is_playing", backend)

    def test_audio_engine_pan_updates_use_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("void SetPan(BorrowedSound sound, float pan) noexcept;", header)
        self.assertGreater(source.count("Platform::AudioPlayback::SetPan("), 0)
        self.assertNotIn("ma_sound_set_pan(", source)
        self.assertIn("ma_sound_set_pan", backend)

    def test_audio_engine_pitch_updates_use_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("void SetPitch(BorrowedSound sound, float pitch) noexcept;", header)
        self.assertGreater(source.count("Platform::AudioPlayback::SetPitch("), 0)
        self.assertNotIn("ma_sound_set_pitch(", source)
        self.assertIn("ma_sound_set_pitch", backend)

    def test_audio_engine_volume_updates_use_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("void SetVolume(BorrowedSound sound, float volume) noexcept;", header)
        self.assertGreater(source.count("Platform::AudioPlayback::SetVolume("), 0)
        self.assertNotIn("ma_sound_set_volume(", source)
        self.assertIn("ma_sound_set_volume", backend)

    def test_audio_engine_fade_milliseconds_updates_use_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("void SetFadeInMilliseconds(", header)
        self.assertIn("std::uint64_t durationMs", header)
        self.assertGreater(source.count("Platform::AudioPlayback::SetFadeInMilliseconds("), 0)
        self.assertNotIn("ma_sound_set_fade_in_milliseconds(", source)
        self.assertIn("ma_sound_set_fade_in_milliseconds", backend)

    def test_audio_engine_fade_pcm_updates_use_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("void SetFadeInPcmFrames(", header)
        self.assertIn("std::uint64_t durationFrames", header)
        self.assertGreater(source.count("Platform::AudioPlayback::SetFadeInPcmFrames("), 0)
        self.assertNotIn("ma_sound_set_fade_in_pcm_frames(", source)
        self.assertIn("ma_sound_set_fade_in_pcm_frames", backend)

    def test_audio_engine_stop_time_updates_use_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("void SetStopTimeInPcmFrames(", header)
        self.assertIn("FrameIndex frame", header)
        self.assertGreater(source.count("Platform::AudioPlayback::SetStopTimeInPcmFrames("), 0)
        self.assertNotIn("ma_sound_set_stop_time_in_pcm_frames(", source)
        self.assertIn("ma_sound_set_stop_time_in_pcm_frames", backend)

    def test_audio_engine_reset_stop_time_and_fade_uses_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("void ResetStopTimeAndFade(BorrowedSound sound) noexcept;", header)
        self.assertGreater(source.count("Platform::AudioPlayback::ResetStopTimeAndFade("), 0)
        self.assertNotIn("ma_sound_reset_stop_time_and_fade(", source)
        self.assertIn("ma_sound_reset_stop_time_and_fade", backend)

    def test_audio_engine_start_commands_use_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("BackendResult Start(BorrowedSound sound) noexcept;", header)
        self.assertGreater(source.count("Platform::AudioPlayback::Start("), 0)
        self.assertNotIn("ma_sound_start(", source)
        self.assertIn("ma_sound_start", backend)

    def test_audio_engine_stop_commands_use_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("BackendResult Stop(BorrowedSound sound) noexcept;", header)
        self.assertGreater(source.count("Platform::AudioPlayback::Stop("), 0)
        self.assertNotIn("ma_sound_stop(", source)
        self.assertIn("ma_sound_stop", backend)

    def test_audio_engine_seek_commands_use_playback_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_playback.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_playback_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")

        self.assertIn("BackendResult SeekToFrame(BorrowedSound sound, FrameIndex frame) noexcept;", header)
        self.assertGreater(source.count("Platform::AudioPlayback::SeekToFrame("), 0)
        self.assertNotIn("ma_sound_seek_to_pcm_frame(", source)
        self.assertIn("ma_sound_seek_to_pcm_frame", backend)

    def test_key_detector_uses_audio_file_boundary_with_key_path_policy(self) -> None:
        source = (ROOT / "src" / "KeyDetector.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "platform/audio_file.h"', source)
        self.assertRegex(
            source,
            r"reader\.Open\(\s*filePath,\s*Platform::AudioFile::LegacyWindowsPathPolicy::"
            r"KeyPermissiveUtf8CanonicalPrefix\s*\)",
        )
        for owned_elsewhere in (
            '#include "platform/text_encoding.h"',
            "Platform::TextEncoding::Utf8ToWidePermissive",
            "KDInitDecoder",
            "ma_decoder_init_file_w",
            "ma_decoder_read_pcm_frames",
            "ma_decoder_seek_to_pcm_frame",
            'L"\\\\?\\',
        ):
            self.assertNotIn(owned_elsewhere, source)

    def test_waapi_trace_uses_filesystem_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WaapiClient.cpp").read_text(encoding="utf-8")

        self.assertIn("std::string TemporaryDirectory();", header)
        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::StreamWriter traceFile;", source)
        self.assertIn("Platform::FileSystem::TemporaryDirectory()", source)
        self.assertIn("traceFile.Open(tracePath, true, false)", source)
        self.assertIn("traceFile.Write", source)
        self.assertIn("traceFile.Flush", source)
        for direct_io in ("std::ofstream", "fs::temp_directory_path", "#include <fstream>"):
            self.assertNotIn(direct_io, source)

    def test_waapi_handshake_is_owned_by_ixwebsocket(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        waapi = (ROOT / "src" / "WaapiClient.cpp").read_text(encoding="utf-8")

        self.assertFalse((ROOT / "src" / "WsHandshakeKey.cpp").exists())
        self.assertFalse((ROOT / "src" / "WsHandshakeKey.h").exists())
        self.assertNotIn("src/WsHandshakeKey.cpp", cmake)

        cpp_sources = "\n".join(
            path.read_text(encoding="utf-8", errors="replace")
            for path in (ROOT / "src").rglob("*")
            if path.suffix.lower() in {".c", ".cc", ".cpp", ".h", ".hpp", ".mm"}
        )
        for retired_api in (
            "WsHandshakeKey",
            "generateClientKeyBase64",
            "computeServerAccept",
            "kRfc6455Guid",
        ):
            self.assertNotIn(retired_api, cpp_sources)

        self.assertIn("#include <ixwebsocket/IXWebSocket.h>", waapi)
        self.assertIn("ix::WebSocket ws;", waapi)
        self.assertIn("impl_->ws.start();", waapi)

    def test_wwise_transfer_copy_uses_filesystem_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WwiseTransferExecute.cpp").read_text(encoding="utf-8")

        self.assertIn("bool IsDirectory(const std::string& pathUtf8);", header)
        self.assertIn("bool CopyFile(", header)
        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::IsDirectory", source)
        self.assertGreaterEqual(source.count("Platform::FileSystem::CreateDirectories"), 2)
        self.assertGreaterEqual(source.count("Platform::FileSystem::CopyFile"), 2)
        for direct_io in ("fs::is_directory", "fs::create_directories", "fs::copy_file"):
            self.assertNotIn(direct_io, source)

    def test_undo_rename_uses_filesystem_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "UndoSystem.cpp").read_text(encoding="utf-8")

        self.assertIn("bool Rename(", header)
        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertGreaterEqual(source.count("Platform::FileSystem::Exists"), 3)
        self.assertGreaterEqual(source.count("Platform::FileSystem::Rename"), 3)
        self.assertNotIn("windows.h", source)
        self.assertNotIn("fs::exists", source)
        self.assertNotIn("fs::rename", source)

    def test_main_ai_model_status_checks_use_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertEqual(2, source.count("const bool selectedModelExists ="))
        self.assertGreaterEqual(
            source.count("Platform::FileSystem::Exists(selectedModelPath)"), 2
        )
        self.assertIn(
            "Platform::FileSystem::Exists(GetAIModelPath(modelNames[i]))", source
        )
        self.assertNotIn("fs::exists(fs::path(selectedModelPath))", source)
        self.assertNotIn(
            "fs::exists(fs::path(GetAIModelPath(modelNames[i])))", source
        )

    def test_wwise_relative_path_uses_filesystem_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WwiseTransferPanel.cpp").read_text(encoding="utf-8")

        self.assertIn("std::string WeaklyCanonical(", header)
        self.assertIn("std::string RelativePath(", header)
        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::WeaklyCanonical", source)
        self.assertIn("Platform::FileSystem::RelativePath", source)
        self.assertNotIn("fs::weakly_canonical", source)
        self.assertNotIn("fs::relative", source)

    def test_wwise_folder_picker_uses_system_pal_with_initial_path(self) -> None:
        source = (ROOT / "src" / "WwiseTransferPanel.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "platform/system.h"', source)
        self.assertIn(
            'return Platform::BrowseFolderDialog(nullptr, "", startPathUtf8);', source
        )
        for native_token in (
            "#if defined(_WIN32)",
            "windows.h",
            "shobjidl.h",
            "IFileDialog",
            "CoCreateInstance",
            "CoInitializeEx",
            "CoTaskMemFree",
            "CoUninitialize",
            "MultiByteToWideChar",
            "WideCharToMultiByte",
        ):
            self.assertNotIn(native_token, source)

    def test_main_file_dialog_com_memory_uses_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("void FreeComTaskMemory(void* handle);", header)
        dialog_body = source.split("std::string OpenFolderDialog(HWND hwnd)", 1)[1].split(
            "// Application State", 1
        )[0]
        self.assertNotIn("Platform::FreeComTaskMemory(pszPath);", dialog_body)
        self.assertNotIn("CoTaskMemFree(pszPath);", dialog_body)
        self.assertIn("#include <ole2.h>", source)

    def test_main_unique_drag_name_exclusive_probe_uses_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static std::string MakeUniqueDragName", 1)[1].split(
            "static void TrackTempDragFile", 1
        )[0]

        self.assertIn("bool CanOpenExclusiveWritePath(", header)
        self.assertIn("bool CanOpenExclusiveWritePath(", win32)
        self.assertIn("bool CanOpenExclusiveWritePath(", macos)
        self.assertIn("Platform::CanOpenExclusiveWritePath(", body)
        self.assertNotIn("CreateFileW(", body)
        self.assertNotIn("CloseHandle(", body)
        self.assertNotIn("GetLastError()", body)

    def test_wwise_query_path_checks_use_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "WwiseQuery.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::Exists", source)
        self.assertIn("Platform::FileSystem::IsDirectory", source)
        self.assertIn("Platform::FileSystem::RelativePath", source)
        self.assertNotIn("fs::exists", source)
        self.assertNotIn("fs::is_directory", source)
        self.assertNotIn("fs::relative", source)
        self.assertIn("Platform::FileSystem::DirectoryIterator", source)
        self.assertIn("Platform::FileSystem::DirectoryReadStatus::End", source)
        self.assertIn("Platform::FileSystem::DirectoryReadStatus::Error", source)
        self.assertNotIn("fs::recursive_directory_iterator", source)

    def test_file_browser_path_state_uses_filesystem_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        encoding_header = (ROOT / "src" / "platform" / "text_encoding.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "FileBrowser.cpp").read_text(encoding="utf-8")

        self.assertIn("std::string CurrentDirectory();", header)
        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::CurrentDirectory", source)
        self.assertIn("Platform::FileSystem::Exists", source)
        self.assertIn("Platform::FileSystem::IsDirectory", source)
        self.assertIn("Platform::FileSystem::WeaklyCanonical", source)
        for direct_io in ("fs::current_path", "fs::exists", "fs::is_directory", "fs::canonical"):
            self.assertNotIn(direct_io, source)
        self.assertGreaterEqual(source.count("Platform::FileSystem::DirectoryIterator"), 2)
        self.assertIn("Platform::FileSystem::DirectoryReadStatus::End", source)
        self.assertIn("Platform::FileSystem::DirectoryReadStatus::Error", source)
        self.assertGreaterEqual(source.count("scan->cancelled.load()"), 4)
        self.assertNotIn("fs::directory_iterator", source)
        self.assertNotIn("fs::recursive_directory_iterator", source)
        self.assertNotIn("fs::relative", source)
        self.assertIn("std::wstring Utf8ToWidePermissive(std::string_view value);", encoding_header)
        self.assertIn("std::string WideToUtf8Permissive(std::wstring_view value);", encoding_header)
        self.assertIn('#include "platform/text_encoding.h"', source)
        utf8_to_path = source.split("static fs::path Utf8ToPath", 1)[1].split(
            "// Supported audio extensions", 1
        )[0]
        path_to_utf8 = source.split("static std::string PathToUtf8", 1)[1].split(
            "// Convert UTF-8 string to filesystem path", 1
        )[0]
        self.assertIn("Platform::TextEncoding::Utf8ToWidePermissive(str)", utf8_to_path)
        self.assertIn("if (wide.empty()) return fs::path(str);", utf8_to_path)
        self.assertIn("Platform::TextEncoding::WideToUtf8Permissive(p.wstring())", path_to_utf8)
        for native_token in (
            "#include <windows.h>", "MultiByteToWideChar", "WideCharToMultiByte",
        ):
            self.assertNotIn(native_token, source)

    def test_naming_script_conflict_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "NamingScript.cpp").read_text(encoding="utf-8")
        body = source.split("std::string NamingScriptEngine::resolveConflict", 1)[1]

        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::ProbePath(utf8Path)", body)
        self.assertIn("Platform::FileSystem::PathStatus::Present", body)
        self.assertIn("std::string candidate = baseName + extension;", body)
        self.assertIn('targetDir + "/" + candidate', body)
        self.assertIn("for (int i = 1; i <= maxAttempts; ++i)", body)
        self.assertIn('baseName + "_" + std::to_string(i) + extension', body)
        for native_token in (
            "#include <windows.h>", "#include <sys/stat.h>",
            "MultiByteToWideChar", "GetFileAttributesW", "::stat(",
        ):
            self.assertNotIn(native_token, source)

    def test_theme_mascot_dormant_click_sound_is_retired(self) -> None:
        header = (ROOT / "src" / "ThemeMascot.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "ThemeMascot.cpp").read_text(encoding="utf-8")
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")

        for dead_token in (
            "#include <windows.h>", "#include <mmsystem.h>",
            "PlaySound", "SND_", "generateToneWav", "playClickSound",
            "m_clickSoundIdx", "winmm.lib", "#include <thread>",
            "#include <vector>", "#include <cstring>",
        ):
            self.assertNotIn(dead_token, header + source)

        for visual_token in (
            "ThemeMascot::getOverdriveColor",
            "ThemeMascot::getOverdriveFillStart",
            "ThemeMascot::getOverdriveFillEnd",
            "ThemeMascot::renderOverdriveEffect",
            "ThemeMascot::updateDragState",
            "ThemeMascot::drawClickFlash",
            "ThemeMascot::renderMascot",
            "m_wasClicked = true",
            "m_clickFlashTimer = CLICK_FLASH_DURATION",
            "m_headTurnTimer = HEAD_TURN_DURATION",
            "m_lookingAtUser = true",
        ):
            self.assertIn(visual_token, source)
        for main_call in (
            "ThemeMascot::getInstance().getOverdriveColor",
            "ThemeMascot::getInstance().getOverdriveFillStart",
            "ThemeMascot::getInstance().getOverdriveFillEnd",
            "ThemeMascot::getInstance().renderOverdriveEffect",
        ):
            self.assertIn(main_call, main)
        self.assertIn("enum class ThemeType", header)
        self.assertIn("enum class MascotAnimState", header)

    def test_filesystem_pal_exposes_incremental_directory_iterator(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")

        for declaration in (
            "enum class DirectoryReadStatus",
            "struct DirectoryEntry",
            "class DirectoryIterator",
            "bool Open(const std::string& rootPathUtf8, bool recursive, bool skipPermissionDenied);",
            "DirectoryReadStatus Next(DirectoryEntry& outEntry,",
            "std::string* errorMessage = nullptr);",
            "bool IsOpen() const;",
            "void Close();",
        ):
            self.assertIn(declaration, header)
        self.assertIn("struct Impl;", header)
        self.assertNotIn("std::filesystem", header)
        self.assertNotIn("recursive_directory_iterator", header)

    def test_theme_trace_uses_platform_exe_dir(self) -> None:
        source = (ROOT / "src" / "Theme.cpp").read_text(encoding="utf-8")
        self.assertIn("Platform::GetExeDir()", source)
        self.assertNotIn("GetExeDirUtf8", source)
        self.assertNotIn("#include <windows.h>", source)

    def test_metadata_atomic_swap_delegates_to_pal(self) -> None:
        source = (ROOT / "src" / "MetadataWriter.cpp").read_text(encoding="utf-8")
        body = source.split("bool MetadataWriter::atomicSwap", 1)[1].split(
            "// General Field Writeback", 1
        )[0]
        self.assertIn("Platform::FileSystem::AtomicFileReplace(originalPath, tempPath)", body)
        for native_call in ("MoveFileExW", "DeleteFileW", "MoveFileW"):
            self.assertNotIn(native_call, body)

    def test_metadata_writer_encoding_uses_text_encoding_pal(self) -> None:
        source = (ROOT / "src" / "MetadataWriter.cpp").read_text(encoding="utf-8", errors="replace")

        self.assertIn('#include "platform/text_encoding.h"', source)
        self.assertIn("Platform::TextEncoding::Utf8ToWidePermissive(str)", source)
        self.assertIn("Platform::TextEncoding::WideToUtf8Permissive(wext)", source)
        self.assertNotIn("MultiByteToWideChar", source)
        self.assertNotIn("WideCharToMultiByte", source)

    def test_pro_tools_bridge_uses_filesystem_pal_for_queue_paths(self) -> None:
        source = (ROOT / "src" / "ProToolsBridge.cpp").read_text(encoding="utf-8", errors="replace")

        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertEqual(4, source.count("Platform::FileSystem::Exists("))
        self.assertEqual(2, source.count("Platform::FileSystem::CreateDirectories("))
        self.assertNotIn("std::filesystem::exists", source)
        self.assertNotIn("std::filesystem::create_directories", source)

    def test_pro_tools_capability_path_probe_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "ProToolsBridge.cpp").read_text(encoding="utf-8")
        body = source.split("bool PathExistsWin32", 1)[1].split(
            "bool EqualsIgnoreCaseAscii", 1
        )[0]
        self.assertIn("if (path.empty())", body)
        self.assertIn("return Platform::FileSystem::Exists(path);", body)
        self.assertNotIn("GetFileAttributesA", body)

    def test_windows_main_font_candidates_use_utf8_path_probe(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("static ImFont* AddFontFromCandidates", 1)[1].split(
            "void LoadChineseFont", 1
        )[0]

        self.assertIn("if (!path || !PathExistsUtf8(path)) continue;", body)
        self.assertNotIn("GetFileAttributesA", body)

    def test_windows_main_waits_use_system_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")

        self.assertEqual(19, source.count("Platform::SleepMilliseconds("))
        self.assertIsNone(re.search(r"(?<![A-Za-z0-9_:])Sleep\(", source))

    def test_main_pro_tools_probe_exists_checks_use_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        guide = source.split("static bool EnsureProToolsSdkGuideReadme", 1)[1].split(
            "static bool IsProToolsBridgeHelperPresent", 1
        )[0]
        helper = source.split("static bool IsProToolsBridgeHelperPresent", 1)[1].split(
            "static std::string GetCapabilityDetail", 1
        )[0]
        self.assertIn(
            "Platform::FileSystem::EnsureDirectoryExists(plan.userSdkDirectoryUtf8)",
            guide,
        )
        self.assertIn("Platform::FileSystem::Exists(plan.sdkGuideReadmePathUtf8)", guide)
        self.assertIn("std::filesystem::u8path(plan.sdkGuideReadmePathUtf8)", guide)
        self.assertIn("Platform::FileSystem::StreamWriter writer;", guide)
        self.assertIn("writer.Open(plan.sdkGuideReadmePathUtf8, false, true)", guide)
        self.assertIn("HarkenSearch::ProTools::BuildUserSdkGuideReadmeText(plan)", guide)
        self.assertIn("const bool wrote = writer.Write(text);", guide)
        self.assertIn("writer.Close();", guide)
        self.assertIn("return wrote;", guide)
        self.assertLess(guide.index("std::filesystem::u8path("), guide.index("Platform::FileSystem::Exists("))
        self.assertLess(guide.index("Platform::FileSystem::Exists("), guide.index("writer.Open("))
        self.assertLess(guide.index("writer.Open("), guide.index("BuildUserSdkGuideReadmeText("))
        self.assertLess(guide.index("writer.Write("), guide.index("writer.Close()"))
        self.assertLess(guide.index("writer.Close()"), guide.index("return wrote;"))
        self.assertIn("Platform::FileSystem::Exists(HarkenSearch::ProTools::ResolveProToolsBridgeHelperPath())", helper)
        self.assertNotIn("std::filesystem::create_directories", guide)
        self.assertNotIn("std::filesystem::exists", guide + helper)
        self.assertNotIn("std::ofstream", guide)

    def test_main_pro_tools_repair_bridge_opens_sdk_directory_through_system_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split('if (ImGui::Button(L(Main_ProToolsRepairBtn))) {', 1)[1].split(
            "if (ImGui::Button(ptResetPresentation.actionLabel.c_str()))", 1
        )[0]

        self.assertIn(
            "Platform::OpenInFileManager(ptPlan.userSdkDirectoryUtf8);",
            body,
        )
        self.assertNotIn("ShellExecuteA", body)

    def test_record_removal_delete_uses_filesystem_pal_with_path_status_semantics(self) -> None:
        source = (ROOT / "src" / "RecordRemovalWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        body = source.split("FileDeletionOutcome DeleteFilesFromSystem(", 1)[1]

        self.assertIn("Platform::FileSystem::ProbePath(paths[i])", body)
        self.assertIn("Platform::FileSystem::PathStatus::Present", body)
        self.assertIn("Platform::FileSystem::PathStatus::Error", body)
        self.assertIn("Platform::FileSystem::RemoveFile(paths[i])", body)
        self.assertNotIn("std::filesystem::exists", body)
        self.assertNotIn("std::filesystem::remove", body)

    def test_main_browser_path_search_uses_filesystem_directory_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split(
            "// 文件浏览器模式：输入的是有效路径则导航过去，否则留在浏览器不切模式", 1
        )[1].split("// 非路径输入：不做搜索也不切模式，保持浏览器", 1)[0]

        self.assertIn("isPath = Platform::FileSystem::IsDirectory(trimmed);", body)
        self.assertNotIn("std::filesystem::is_directory", body)
        self.assertNotIn("#ifdef _WIN32", body)

    def test_main_custom_avatar_removal_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split(
            "if (ImGui::Button(L(Set_AvatarRemoveLabelZh)))", 1
        )[1].split("if (ImGui::Button(settingsAvatarPickerOpenLabel.c_str()))", 1)[0]

        remove_call = "Platform::FileSystem::RemoveFile(avatarPath);"
        self.assertIn(remove_call, body)
        self.assertNotIn("std::filesystem::remove", body)
        self.assertLess(
            body.index(remove_call), body.index("CoverModule::EvictCoverTextureByImagePath")
        )
        self.assertLess(
            body.index(remove_call), body.index("state.settings.hasCustomAvatar = false;")
        )

    def test_main_thesaurus_save_preserves_binary_stream_semantics_through_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static void SaveThesaurusJson()", 1)[1].split(
            "static void LoadThesaurusJson()", 1
        )[0]

        self.assertIn("if (thes.readOnly) return;", body)
        self.assertIn("WideToUtf8Path(thes.activePath)", body)
        self.assertNotIn("canonicalPath", body)
        self.assertIn("Platform::FileSystem::StreamWriter writer;", body)
        self.assertIn("writer.Open(path, false, true)", body)
        self.assertIn("writer.Write(GenerateThesaurusJson(g_userThesaurus))", body)
        self.assertNotIn("std::ofstream", body)
        self.assertLess(body.index("writer.Open("), body.index("GenerateThesaurusJson("))

    def test_main_key_value_load_reads_text_through_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split(
            "static std::unordered_map<std::string, std::string> LoadKeyValueFile", 1
        )[1].split("static bool SaveAppSettingsKeyValueFile", 1)[0]

        read_call = "Platform::FileSystem::ReadFileText(pathUtf8, contents)"
        self.assertIn(read_call, body)
        self.assertIn("std::istringstream in(contents)", body)
        self.assertNotIn("std::ifstream", body)
        self.assertNotIn("Utf8ToPathMain", body)
        stream = "std::istringstream in(contents)"
        self.assertLess(body.index(read_call), body.index(stream))
        self.assertLess(body.index(stream), body.index("std::getline(in, line)"))

    def test_main_default_config_directory_creation_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static bool ResolveUserDefaultConfigPaths", 1)[1].split(
            "static bool PublishCurrentDevConfigToUserDefaults", 1
        )[0]

        create_call = "Platform::FileSystem::CreateDirectories(appDataRoot)"
        self.assertIn(create_call, body)
        self.assertNotIn("fs::create_directories", body)
        self.assertNotIn("std::error_code", body)
        self.assertLess(body.index(create_call), body.index("outSettingsPath ="))

    def test_main_dev_config_publish_uses_overwriting_copy_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static bool PublishCurrentDevConfigToUserDefaults", 1)[
            1
        ].split("static int64_t PathMtimeUnixSecondsMain", 1)[0]

        create_call = (
            "Platform::FileSystem::CreateDirectories("
            "PathToUtf8Main(userSettingsFs.parent_path()))"
        )
        settings_copy = (
            "g_appSettingsPathUtf8, userSettingsPath, true, &copyError"
        )
        imgui_copy = "g_imguiIniPathUtf8, userImguiPath, true, &copyError"
        self.assertIn(create_call, body)
        self.assertEqual(2, body.count("Platform::FileSystem::CopyFile("))
        self.assertIn(settings_copy, body)
        self.assertIn(imgui_copy, body)
        self.assertLess(body.index(settings_copy), body.index(imgui_copy))
        first_copy_branch = body[
            body.index(settings_copy) : body.index(imgui_copy)
        ]
        self.assertIn("return false;", first_copy_branch)
        second_copy_branch = body[
            body.index(imgui_copy) : body.index("Dev_PublishSuccess")
        ]
        self.assertIn("return false;", second_copy_branch)
        for direct_io in ("fs::create_directories", "fs::copy_file", "std::error_code"):
            self.assertNotIn(direct_io, body)

    def test_main_wav_bit_depth_uses_incremental_stream_reader_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static int GetWavBitDepthUtf8", 1)[1].split(
            "// Custom ImGui Pitch Wheel Widget", 1
        )[0]

        self.assertIn("Platform::FileSystem::StreamReader file;", body)
        self.assertIn("file.Open(filePathUtf8, true)", body)
        self.assertIn("file.Read(header, sizeof(header))", body)
        self.assertIn("file.Read(id, sizeof(id))", body)
        self.assertIn("file.Read(sizeData, sizeof(sizeData))", body)
        self.assertIn("file.Read(fmt.data(), fmt.size())", body)
        self.assertIn("Platform::FileSystem::StreamSeekOrigin::Current", body)
        self.assertNotIn("std::ifstream", body)
        self.assertNotIn("Utf8ToPathMain", body)

    def test_main_folder_name_guess_uses_directory_iterator_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static std::string GuessFolderNameByNamingTag", 1)[
            1
        ].split("static void BuildFolderRenamePreview", 1)[0]

        self.assertIn("Platform::FileSystem::DirectoryIterator iterator;", body)
        self.assertIn(
            'if (!iterator.Open(PathToUtf8Main(folder), scanRecursive, false)) return "";',
            body,
        )
        self.assertIn("Platform::FileSystem::DirectoryReadStatus::End", body)
        self.assertIn(
            'if (status == Platform::FileSystem::DirectoryReadStatus::Error) return "";',
            body,
        )
        self.assertIn("entry.isRegularFile", body)
        self.assertIn("Utf8ToPathMain(entry.pathUtf8)", body)
        self.assertNotIn("fs::directory_iterator", body)
        self.assertNotIn("fs::recursive_directory_iterator", body)

    def test_main_folder_rename_preview_scan_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static void BuildFolderRenamePreview", 1)[1].split(
            "// Helper: Remove file extension", 1
        )[0]
        scan_body = body.split("// conflict detection", 1)[0]

        self.assertRegex(
            scan_body,
            r"Platform::FileSystem::ProbePath\(rootUtf8\)\s*!=\s*"
            r"Platform::FileSystem::PathStatus::Present",
        )
        self.assertIn("Platform::FileSystem::IsDirectory(rootUtf8)", scan_body)
        self.assertIn("Platform::FileSystem::DirectoryIterator iterator;", scan_body)
        self.assertIn(
            "iterator.Open(rootUtf8, state.folderRenameRecursiveFolders, false)",
            scan_body,
        )
        self.assertIn("Platform::FileSystem::DirectoryReadStatus::End", scan_body)
        self.assertIn("Platform::FileSystem::DirectoryReadStatus::Error", scan_body)
        self.assertIn("state.folderRenameLog = L(FolderExec_ScanFailed);", scan_body)
        self.assertIn("if (!entry.isDirectory) continue;", scan_body)
        self.assertIn("folders.push_back(Utf8ToPathMain(entry.pathUtf8));", scan_body)
        for direct_io in (
            "fs::exists",
            "fs::is_directory",
            "fs::directory_iterator",
            "fs::recursive_directory_iterator",
        ):
            self.assertNotIn(direct_io, scan_body)

    def test_rename_conflict_checks_use_filesystem_pal_at_their_owners(self) -> None:
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        policy = (ROOT / "src/BatchRenamePolicy.cpp").read_text(encoding="utf-8")
        workflow = (ROOT / "src/RecordRenameWorkflow.cpp").read_text(
            encoding="utf-8"
        )

        self.assertIn(
            "Platform::FileSystem::Exists(PathToUtf8Main(newPath))", main
        )
        self.assertNotIn("fs::exists(newPath)", main)
        self.assertIn(
            "Platform::FileSystem::Exists(PathToUtf8(targetPath))", policy
        )
        self.assertNotIn("fs::exists(targetPath)", policy)
        for path_name in ("oldP", "newP"):
            self.assertIn(
                f"Platform::FileSystem::Exists(PathToUtf8({path_name}))", workflow
            )
            self.assertNotIn(f"fs::exists({path_name})", workflow)

    def test_database_watcher_event_entry_uses_filesystem_path_probe(self) -> None:
        source = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = source.split("void Database::handleFileSystemEvent", 1)[1].split(
            "if (isDeleted)", 1
        )[0]

        self.assertRegex(
            body,
            r"Platform::FileSystem::ProbePath\(filePath\)\s*!=\s*"
            r"Platform::FileSystem::PathStatus::Present",
        )
        self.assertIn("Platform::FileSystem::IsDirectory(filePath)", body)
        self.assertNotIn("Utf8ToPath", body)
        self.assertNotIn("std::error_code", body)
        self.assertNotIn("fs::exists", body)
        self.assertNotIn("fs::is_directory", body)

    def test_database_watcher_add_reads_file_info_through_pal(self) -> None:
        source = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = source.split("void Database::handleFileSystemEvent", 1)[1]
        add_body = body.split("if (isAdded)", 1)[1].split("if (isModified)", 1)[0]

        probe = "Platform::FileSystem::ProbeRegularFile(filePath)"
        mtime = "Platform::FileSystem::LastWriteTimeUnixSeconds(filePath)"
        self.assertIn(probe, add_body)
        self.assertIn("fileInfo.sizeAvailable ? fileInfo.sizeBytes : 0", add_body)
        self.assertIn(mtime, add_body)
        self.assertLess(add_body.index(probe), add_body.index("record.fileSize"))
        self.assertLess(add_body.index("record.fileSize"), add_body.index(mtime))
        self.assertNotIn("fs::file_size", add_body)
        self.assertNotIn("fs::last_write_time", add_body)
        self.assertNotIn("std::error_code", add_body)

    def test_database_watcher_modify_reads_file_size_through_pal(self) -> None:
        source = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = source.split("void Database::handleFileSystemEvent", 1)[1].split(
            "void Database::interrupt", 1
        )[0]
        modify_body = body.split("if (isModified)", 1)[1]

        probe = "Platform::FileSystem::ProbeRegularFile(filePath)"
        self.assertIn(probe, modify_body)
        self.assertRegex(
            modify_body,
            r"modifiedFileInfo\.sizeAvailable\s*\?\s*"
            r"modifiedFileInfo\.sizeBytes\s*:\s*"
            r"std::numeric_limits<uint64_t>::max\(\)",
        )
        self.assertLess(modify_body.index(probe), modify_body.index("existing.fileSize"))
        self.assertNotIn("fs::file_size", modify_body)

    def test_main_thesaurus_load_uses_filesystem_pal_with_legacy_fallback(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static void LoadThesaurusJson() {", 1)[1].split(
            "static std::vector<std::string> SplitThesaurusWords", 1
        )[0]

        ensure_call = "Platform::HarkenUserData::EnsureThesaurusFileUtf8(GetExeDirUtf8())"
        read_call = "Platform::FileSystem::ReadFileText(path, json)"
        # Scene 1: PAL load — single filesystem PAL read of EnsureThesaurusFileUtf8().activePath
        self.assertIn(ensure_call, body)
        self.assertIn("thes.activePath", body)
        self.assertEqual(1, body.count(read_call))
        self.assertLess(body.index(ensure_call), body.index("thes.activePath"))
        self.assertLess(body.index("thes.activePath"), body.index(read_call))
        # Scene 2: legacy-name fallback still exists, now owned by EnsureThesaurusFileUtf8
        migrator = (ROOT / "src" / "platform" / "harken_legacy_data_migrator.cpp").read_text(
            encoding="utf-8"
        )
        ensure_body = migrator.split("ThesaurusFile EnsureThesaurusFileUtf8", 1)[1].split(
            "std::wstring LegacyAuroDbFilePath", 1
        )[0]
        self.assertIn("LegacyArisThesaurusFileName()", ensure_body)
        self.assertIn("LegacyAuroThesaurusFileName()", ensure_body)
        self.assertIn("LayoutKind::FallbackLegacy", ensure_body)
        self.assertLess(
            ensure_body.index("LegacyArisThesaurusFileName()"),
            ensure_body.index("LegacyAuroThesaurusFileName()"),
        )
        self.assertLess(
            ensure_body.index("LegacyAuroThesaurusFileName()"),
            ensure_body.index("LayoutKind::FallbackLegacy"),
        )
        self.assertIn("if (loaded)", body)
        self.assertLess(body.index("if (loaded)"), body.index("g_userThesaurus.clear();"))
        self.assertNotIn("std::ifstream", body)
        self.assertNotIn("in.open", body)

    def test_main_thesaurus_merge_reads_selected_wide_path_through_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        backend = (
            ROOT / "src" / "platform" / "thesaurus_json_dialog_win32.cpp"
        ).read_text(encoding="utf-8")
        body = source.split("if (ImGui::Button(L(Set_ThesMerge)", 1)[1].split(
            "if (ImGui::Button(L(Set_ThesResetDefault)", 1
        )[0]

        capability_call = (
            "Platform::ThesaurusDialogs::OpenThesaurusJsonFileDialogUtf8(g_hwnd)"
        )
        read_call = "Platform::FileSystem::ReadFileText(importPath, json)"
        self.assertIn(
            "TextEncoding::WideToUtf8(std::wstring(szFileName))", backend
        )
        self.assertIn(capability_call, body)
        self.assertIn(read_call, body)
        self.assertLess(body.index(capability_call), body.index(read_call))
        self.assertLess(body.index(read_call), body.index("ParseThesaurusJson(json, imported)"))
        self.assertLess(body.index("ParseThesaurusJson(json, imported)"), body.index("g_userThesaurus.insert"))
        self.assertLess(body.index("g_userThesaurus.insert"), body.index("SaveThesaurusJson();"))
        self.assertNotIn("WideToUtf8", body)
        self.assertNotIn("std::ifstream", body)
        self.assertNotIn("std::istreambuf_iterator", body)

    def test_main_dev_mode_marker_creation_uses_filesystem_text_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("if (ImGui::Button(L(Set_CreateDevMode)", 1)[1].split(
            "if (ImGui::Button(L(Set_OpenExeDir)", 1
        )[0]

        self.assertIn(
            'Platform::FileSystem::WriteFileText(devModePath, "# Aris dev mode\\n");',
            body,
        )
        self.assertNotIn("std::ofstream", body)

    def test_main_dev_mode_marker_removal_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("if (ImGui::Button(L(Set_DeleteDevMode)", 1)[1].split(
            "if (ImGui::Button(L(Set_CreateDevMode)", 1
        )[0]

        self.assertIn("Platform::FileSystem::RemoveFile(devModePath);", body)
        self.assertNotIn("fs::remove", body)
        self.assertNotIn("std::error_code", body)

    def test_main_theme_trace_uses_filesystem_append_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static void AppendThemeTraceLineMain", 1)[1].split(
            "static void LogThemeStockSwitchSnapshot", 1
        )[0]

        self.assertIn("#if AURA_INTERNAL_BETA", body)
        self.assertIn("Platform::FileSystem::AppendFileText(", body)
        self.assertIn('GetExeDirUtf8() + "\\\\theme_path_trace.log"', body)
        self.assertIn("line + '\\n'", body)
        self.assertNotIn("std::ofstream", body)
        self.assertIn("catch (...)", body)

    def test_main_key_value_save_uses_binary_stream_pal_without_format_change(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static bool SaveAppSettingsKeyValueFile", 1)[1].split(
            "// Helper:", 1
        )[0]

        self.assertIn("Platform::FileSystem::StreamWriter writer;", body)
        self.assertIn("writer.Open(pathUtf8, false, true)", body)
        self.assertIn('writer.Write("# Aris settings (auto-generated)\\n");', body)
        for write in (
            "writer.Write(it.first)",
            'writer.Write("=")',
            "writer.Write(it.second)",
            'writer.Write("\\n")',
        ):
            self.assertIn(write, body)
        self.assertNotIn("std::ofstream", body)

    def test_main_failed_ai_model_load_removes_cache_through_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split(
            "std::thread([&state, selectedModelExists, modelPath, selectedModelName]()", 1
        )[1].split("}).detach();", 1)[0]

        remove_call = "Platform::FileSystem::RemoveFile(modelPath);"
        self.assertIn(remove_call, body)
        self.assertNotIn("fs::remove", body)
        self.assertNotIn("std::error_code", body)
        self.assertLess(body.index(remove_call), body.index("GetAIEngine().DownloadModel"))

    def test_main_thesaurus_reset_removes_current_and_legacy_files_through_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("if (ImGui::Button(L(Set_ThesResetDefault)", 1)[1].split(
            "ImGui::Spacing();", 1
        )[0]

        self.assertIn("Platform::FileSystem::RemoveFile", body)
        self.assertIn("CanonicalThesaurusFileName", body)
        self.assertIn("LegacyArisThesaurusFileName", body)
        self.assertIn("LegacyAuroThesaurusFileName", body)
        self.assertNotIn("fs::remove", body)
        self.assertNotIn("std::error_code", body)
        self.assertLess(body.index("CanonicalThesaurusFileName"), body.index("LegacyArisThesaurusFileName"))
        self.assertLess(body.index("LegacyAuroThesaurusFileName"), body.index("LoadThesaurusJson();"))

    def test_main_release_notes_read_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static void LoadReleaseNotesFromExeDir(bool forceReload) {", 1)[1].split(
            "static bool IsSendToDawSuccessStatus", 1
        )[0]
        self.assertIn("Platform::FileSystem::ReadFileText(notesPath, jsonText)", body)
        self.assertNotIn("std::ifstream", body)
        self.assertNotIn("in.open", body)

    def test_pro_tools_bridge_uses_filesystem_pal_for_temp_directory(self) -> None:
        source = (ROOT / "src" / "ProToolsBridge.cpp").read_text(encoding="utf-8", errors="replace")

        self.assertEqual(2, source.count("Platform::FileSystem::TemporaryDirectory()"))
        self.assertNotIn("std::filesystem::temp_directory_path", source)

    def test_pro_tools_bridge_uses_filesystem_pal_for_json_io(self) -> None:
        source = (ROOT / "src" / "ProToolsBridge.cpp").read_text(encoding="utf-8", errors="replace")

        self.assertEqual(2, source.count("Platform::FileSystem::ReadFileText("))
        self.assertEqual(2, source.count("Platform::FileSystem::WriteFileText("))
        self.assertNotIn("std::ifstream", source)
        self.assertNotIn("std::ofstream", source)

    def test_pro_tools_session_cache_cleanup_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "ProToolsBridge.cpp").read_text(encoding="utf-8", errors="replace")
        body = source.split("void ClearProToolsSessionCache()", 1)[1].split(
            "std::string DefaultUserSdkDirectory()", 1
        )[0]

        self.assertIn(
            "Platform::FileSystem::RemoveFile(ProToolsSessionCachePath().u8string())",
            body,
        )
        self.assertNotIn("std::filesystem::remove", body)
        retired = {
            "src/ProToolsBridge.cpp|filesystem-io|std::filesystem::remove|43fe3d5a59ffb7fa|1",
        }
        current = {finding.fingerprint for finding in boundary.scan_tree(ROOT)}
        self.assertTrue(retired.isdisjoint(current), retired & current)

    def test_pro_tools_process_enumeration_uses_process_query_pal(self) -> None:
        source = (ROOT / "src" / "ProToolsBridge.cpp").read_text(
            encoding="utf-8", errors="replace"
        )
        header_path = ROOT / "src" / "platform" / "process_query.h"
        win32_path = ROOT / "src" / "platform" / "process_query_win32.cpp"
        unsupported_path = ROOT / "src" / "platform" / "process_query_unsupported.cpp"
        for path in (header_path, win32_path, unsupported_path):
            self.assertTrue(path.is_file(), f"missing process-query PAL file: {path}")
        header = header_path.read_text(encoding="utf-8")
        win32 = win32_path.read_text(encoding="utf-8")
        unsupported = unsupported_path.read_text(encoding="utf-8")

        self.assertIn('#include "platform/process_query.h"', source)
        self.assertEqual(3, source.count("Platform::FindProcessIdsByImageName("))
        self.assertNotIn("#include <tlhelp32.h>", source)
        self.assertNotIn("PROCESSENTRY32W", source)
        self.assertNotIn("CreateToolhelp32Snapshot", source)
        self.assertIn("std::vector<uint32_t> FindProcessIdsByImageName(", header)
        self.assertIn("ProcessTerminationResult TerminateProcessById(", header)
        for token in ("windows.h", "HANDLE", "DWORD", "PROCESSENTRY32W"):
            self.assertNotIn(token, header)
        for token in (
            "CreateToolhelp32Snapshot",
            "Process32FirstW",
            "Process32NextW",
            "CloseHandle(snapshot)",
        ):
            self.assertIn(token, win32)
        for token in ("OpenProcess", "TerminateProcess", "WaitForSingleObject"):
            self.assertIn(token, win32)
        self.assertIn("ProcessTerminationStatus::Unsupported", unsupported)
        terminate_body = source.split("int TerminateProcessesByImageName", 1)[1].split(
            "struct CloseProToolsWindowContext", 1
        )[0]
        self.assertIn("Platform::TerminateProcessById(", terminate_body)
        for token in ("OpenProcess(", "TerminateProcess(", "WaitForSingleObject(", "CloseHandle("):
            self.assertNotIn(token, terminate_body)
        self.assertIn("return {};", unsupported)

    def test_pro_tools_current_process_id_uses_system_pal(self) -> None:
        source = (ROOT / "src" / "ProToolsBridge.cpp").read_text(
            encoding="utf-8", errors="replace"
        )
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        helper_target = cmake.split("add_executable(HarkenSearchProToolsBridge", 1)[1].split(
            ")", 1
        )[0]
        body = source.split("int TerminateProcessesByImageName", 1)[1].split(
            "struct CloseProToolsWindowContext", 1
        )[0]

        self.assertIn('#include "platform/system.h"', source)
        self.assertIn("Platform::CurrentProcessId()", body)
        self.assertNotIn("GetCurrentProcessId()", body)
        self.assertIn("src/platform/system_win32.cpp", helper_target)

    def test_pro_tools_window_probe_uses_system_window_pal(self) -> None:
        source = (ROOT / "src" / "ProToolsBridge.cpp").read_text(
            encoding="utf-8", errors="replace"
        )
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        close_body = source.split("bool RequestCloseProToolsWindow", 1)[1].split(
            "int RequestCloseProToolsAfterUserSaved", 1
        )[0]
        probe_body = source.split("bool EnumProbeWindow", 1)[1].split(
            "#endif", 1
        )[0]

        self.assertIn("bool WindowIsValid(void* windowHandle);", header)
        self.assertIn("bool WindowIsValid(void* windowHandle)", win32)
        self.assertIn("IsWindow(static_cast<HWND>(windowHandle)) != FALSE", win32)
        self.assertIn("bool WindowIsValid(void* /*windowHandle*/)", macos)
        self.assertIn("bool WindowIsVisible(void* windowHandle);", header)
        self.assertIn("bool WindowIsVisible(void* windowHandle)", win32)
        self.assertIn("IsWindowVisible(static_cast<HWND>(windowHandle)) != FALSE", win32)
        self.assertIn("bool WindowIsVisible(void* /*windowHandle*/)", macos)
        self.assertIn("return false;", macos)
        self.assertIn("void RequestWindowClose(void* windowHandle);", header)
        self.assertIn("void RequestWindowClose(void* windowHandle)", win32)
        self.assertIn("void RequestWindowClose(void* /*windowHandle*/)", macos)
        self.assertIn("int WindowClassName(void* windowHandle, char* buffer, int capacity);", header)
        self.assertIn("int WindowClassName(void* windowHandle, char* buffer, int capacity)", win32)
        self.assertIn("int WindowClassName(void* /*windowHandle*/, char* /*buffer*/, int /*capacity*/)", macos)
        self.assertIn("Platform::WindowIsValid(windowHandle)", close_body)
        self.assertIn("Platform::WindowProcessId(windowHandle)", close_body)
        self.assertIn("Platform::WindowIsVisible(windowHandle)", close_body)
        self.assertIn("Platform::WindowIsVisible(windowHandle)", probe_body)
        self.assertIn("Platform::RequestWindowClose(windowHandle)", close_body)
        self.assertIn("Platform::WindowClassName(windowHandle, className,", probe_body)
        self.assertNotIn("#include <windows.h>", source)
        self.assertNotIn("PostMessageW", source)
        self.assertNotIn("GetClassNameA", source)
        self.assertIn("Platform::EnumerateTopLevelWindows(", source)
        self.assertNotIn("EnumWindows(", source)
        self.assertNotIn("IsWindow(hwnd)", close_body)
        self.assertNotIn("GetWindowThreadProcessId(hwnd, &pid)", close_body)
        self.assertNotIn("IsWindowVisible(hwnd)", close_body)
        self.assertNotIn("IsWindowVisible(hwnd)", probe_body)

    def test_pro_tools_response_poll_uses_system_sleep_pal(self) -> None:
        source = (ROOT / "src" / "ProToolsBridge.cpp").read_text(
            encoding="utf-8", errors="replace"
        )
        body = source.split("bool WaitForProToolsBridgeResponse", 1)[1].split(
            "LivePtslDispatchResult DispatchLivePtslPersistentRequest", 1
        )[0]

        self.assertIn("Platform::SleepMilliseconds(50);", body)
        self.assertNotRegex(body, r"(?<!::)\bSleep\(50\);")

    def test_pro_tools_bridge_launch_uses_process_pal(self) -> None:
        source = (ROOT / "src" / "ProToolsBridge.cpp").read_text(
            encoding="utf-8", errors="replace"
        )
        start_body = source.split("bool StartPersistentProToolsBridgeServer", 1)[1].split(
            "bool WaitForProToolsBridgeResponse", 1
        )[0]
        self.assertIn("#ifdef _WIN32", start_body)
        self.assertIn(
            "Platform::FileSystem::CreateDirectories(queueDir.u8string());", start_body
        )
        self.assertIn(
            'const std::string args = "--server --queue \\\"" + queueDir.u8string() + "\\\"";',
            start_body,
        )
        self.assertIn("Platform::LaunchProcess(helperPathUtf8, args)", start_body)
        self.assertIn(
            'detail = "HarkenSearchProToolsBridge.exe persistent server failed to start";',
            start_body,
        )
        self.assertIn("#else", start_body)
        self.assertIn(
            'detail = "HarkenSearchProToolsBridge persistent server is Windows-only";',
            start_body,
        )
        self.assertIn("return false;", start_body)
        self.assertNotIn("CreateProcessA", start_body)
        self.assertNotIn("WaitForSingleObject", start_body)
        self.assertNotIn("CloseHandle", start_body)
        self.assertNotIn("HANDLE", start_body)

    def test_pro_tools_helper_dispatch_wait_uses_process_pal(self) -> None:
        source = (ROOT / "src" / "ProToolsBridge.cpp").read_text(
            encoding="utf-8", errors="replace"
        )
        body = source.split("LivePtslDispatchResult DispatchLivePtslHelper", 1)[1].split(
            "LivePtslDispatchResult DispatchLivePtslSmokeTest", 1
        )[0]
        self.assertIn("#include \"platform/process_launch.h\"", source)
        self.assertIn("Platform::RunProcessAndWait(", body)
        self.assertNotIn("CreateProcessA", body)
        self.assertNotIn("WaitForSingleObject", body)
        self.assertNotIn("TerminateProcess", body)
        self.assertNotIn("PROCESS_INFORMATION", body)

    def test_pro_tools_helper_request_response_io_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "HarkenSearchProToolsBridgeMain.cpp").read_text(encoding="utf-8", errors="replace")
        write_body = source.split("bool WriteResponse", 1)[1].split("std::string ReadTextFile", 1)[0]
        read_body = source.split("std::string ReadTextFile", 1)[1].split("std::string JsonEscape", 1)[0]

        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::WriteFileText", write_body)
        self.assertIn("Platform::FileSystem::ReadFileText", read_body)
        self.assertNotIn("std::ofstream", write_body)
        self.assertNotIn("std::ifstream", read_body)

    def test_pro_tools_helper_session_cache_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "HarkenSearchProToolsBridgeMain.cpp").read_text(encoding="utf-8", errors="replace")
        read_body = source.split("std::string ReadCachedPtslSessionId", 1)[1].split(
            "void WriteCachedPtslSessionId", 1
        )[0]
        write_body = source.split("void WriteCachedPtslSessionId", 1)[1].split(
            "void ClearCachedPtslSessionId", 1
        )[0]

        self.assertIn("Platform::FileSystem::ReadFileText", read_body)
        self.assertIn("Platform::FileSystem::WriteFileText", write_body)
        self.assertNotIn("std::ifstream", read_body)
        self.assertNotIn("std::ofstream", write_body)

    def test_pro_tools_helper_queue_directory_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "HarkenSearchProToolsBridgeMain.cpp").read_text(encoding="utf-8", errors="replace")
        body = source.split("int RunPersistentQueueServer", 1)[1].split("// M9:", 1)[0]

        self.assertIn("Platform::FileSystem::CreateDirectories(queueDirUtf8)", body)
        self.assertNotIn("std::filesystem::create_directories", body)

    def test_pro_tools_helper_queue_polling_uses_filesystem_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "HarkenSearchProToolsBridgeMain.cpp").read_text(
            encoding="utf-8", errors="replace"
        )
        self.assertIn("bool RemoveFile(const std::string& pathUtf8", header)
        body = source.split("int RunPersistentQueueServer", 1)[1].split(
            "std::this_thread::sleep_for(std::chrono::milliseconds(50))", 1
        )[0]
        self.assertIn("Platform::FileSystem::DirectoryIterator", body)
        self.assertIn("Platform::FileSystem::DirectoryReadStatus::Error", body)
        self.assertIn("throw std::runtime_error", body)
        self.assertEqual(4, body.count("Platform::FileSystem::RemoveFile("))
        self.assertNotIn("std::filesystem::directory_iterator", body)
        self.assertNotIn("std::filesystem::remove", body)

    def test_pro_tools_import_path_uses_dedicated_encoding_boundary(self) -> None:
        header = ROOT / "src" / "platform" / "pro_tools_path_encoding.h"
        win32 = ROOT / "src" / "platform" / "pro_tools_path_encoding_win32.cpp"
        std = ROOT / "src" / "platform" / "pro_tools_path_encoding_std.cpp"
        source = (ROOT / "src" / "HarkenSearchProToolsBridgeMain.cpp").read_text(encoding="utf-8")
        for path in (header, win32, std):
            self.assertTrue(path.is_file(), f"missing Pro Tools path backend: {path.name}")
        header_text = header.read_text(encoding="utf-8")
        self.assertIn("namespace Platform::ProTools::PathEncoding", header_text)
        self.assertIn("std::string ToImportPath(", header_text)
        self.assertNotIn("text_encoding.h", win32.read_text(encoding="utf-8"))
        win32_text = win32.read_text(encoding="utf-8")
        self.assertIn("std::numeric_limits<int>::max()", win32_text)
        self.assertIn("const int inputLen", win32_text)
        self.assertIn('#include "platform/pro_tools_path_encoding.h"', source)
        body = source.split("std::string Utf8PathForPtslImport", 1)[1].split(
            "bool WriteResponse", 1
        )[0]
        self.assertIn("Platform::ProTools::PathEncoding::ToImportPath", body)
        for token in ("MultiByteToWideChar", "WideCharToMultiByte", "CP_ACP"):
            self.assertNotIn(token, body)
        self.assertNotIn("#include <windows.h>", source)

    def test_pro_tools_mid_path_diagnostic_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "HarkenSearchProToolsBridgeMain.cpp").read_text(encoding="utf-8", errors="replace")
        body = source.split("void DumpMidPathDiagnostic", 1)[1].split(
            "// M10:", 1
        )[0]

        self.assertIn("Platform::FileSystem::WriteFileText(path, out.str())", body)
        self.assertIn("std::ostringstream out", body)
        self.assertNotIn("std::ofstream", body)

    def test_pro_tools_last_send_diagnostic_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "HarkenSearchProToolsBridgeMain.cpp").read_text(encoding="utf-8", errors="replace")
        body = source.split("void DumpLastSendState", 1)[1].split(
            "bool ExecuteMidSpotPath", 1
        )[0]

        self.assertIn("Platform::FileSystem::WriteFileText(path, out.str())", body)
        self.assertIn("std::ostringstream out", body)
        self.assertNotIn("std::ofstream", body)

    def test_pro_tools_resolver_dump_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "HarkenSearchProToolsBridgeMain.cpp").read_text(encoding="utf-8", errors="replace")
        body = source.split("bool ResolveTimelineTargetAudioTrack", 1)[1].split(
            "bool ExecuteMidSpotPath", 1
        )[0]

        self.assertIn("Platform::FileSystem::WriteFileText(dumpPath, dump.str())", body)
        self.assertIn("std::ostringstream dump", body)
        self.assertNotIn("std::ofstream dump", body)

    def test_pro_tools_helper_startup_marker_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "HarkenSearchProToolsBridgeMain.cpp").read_text(encoding="utf-8", errors="replace")
        body = source.split("int RunPersistentQueueServer", 1)[1].split(
            "PTSLC_CPP::ClientConfig", 1
        )[0]

        self.assertIn("Platform::FileSystem::WriteFileText(markerPath, marker.str())", body)
        self.assertIn("std::ostringstream marker", body)
        self.assertNotIn("std::ofstream m", body)

    def test_metadata_remove_album_art_uses_atomic_swap(self) -> None:
        source = (ROOT / "src" / "MetadataWriter.cpp").read_text(encoding="utf-8")
        body = source.split("bool MetadataWriter::removeAlbumArt", 1)[1].split(
            "int MetadataWriter::writeAlbumArtBatch", 1
        )[0]

        self.assertGreaterEqual(
            body.count('atomicSwap(audioFilePath, audioFilePath + ".tmp")'), 4
        )
        for direct_swap in ("DeleteFileW", "MoveFileW", "fs::remove", "fs::rename"):
            self.assertNotIn(direct_swap, body)

    def test_metadata_remove_album_art_streams_use_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "MetadataWriter.cpp").read_text(encoding="utf-8")
        body = source.split("bool MetadataWriter::removeAlbumArt", 1)[1].split(
            "int MetadataWriter::writeAlbumArtBatch", 1
        )[0]

        self.assertIn("ReadBinaryFile", body)
        self.assertIn("WriteBinaryFile", body)
        self.assertIn("Platform::FileSystem::StreamReader", source)
        self.assertIn("Platform::FileSystem::StreamWriter", source)
        self.assertNotIn("std::ifstream", body)
        self.assertNotIn("std::ofstream", body)

    def test_metadata_id3v2_album_art_uses_atomic_swap(self) -> None:
        source = (ROOT / "src" / "MetadataWriter.cpp").read_text(encoding="utf-8")
        body = source.split("bool MetadataWriter::writeID3v2AlbumArt", 1)[1].split(
            "bool MetadataWriter::writeFLACAlbumArt", 1
        )[0]

        self.assertIn('atomicSwap(filePath, filePath + ".tmp")', body)
        self.assertIn("ReadBinaryFile(filePath, fileData)", body)
        self.assertIn("WriteBinaryFile(filePath + \".tmp\"", body)
        self.assertNotIn("std::ifstream", body)
        self.assertNotIn("std::ofstream", body)
        for direct_swap in ("DeleteFileW", "MoveFileW", "fs::remove", "fs::rename"):
            self.assertNotIn(direct_swap, body)

    def test_metadata_flac_album_art_uses_atomic_swap(self) -> None:
        source = (ROOT / "src" / "MetadataWriter.cpp").read_text(encoding="utf-8")
        body = source.split("bool MetadataWriter::writeFLACAlbumArt", 1)[1].split(
            "bool MetadataWriter::writeM4AAlbumArt", 1
        )[0]

        self.assertIn('atomicSwap(filePath, filePath + ".tmp")', body)
        self.assertIn("ReadBinaryFile(filePath, fileData)", body)
        self.assertIn("WriteBinaryFile(filePath + \".tmp\"", body)
        self.assertNotIn("std::ifstream", body)
        self.assertNotIn("std::ofstream", body)
        self.assertIn("ReadBinaryFile(filePath, fileData)", body)
        self.assertIn("WriteBinaryFile(filePath + \".tmp\"", body)
        self.assertNotIn("std::ifstream", body)
        self.assertNotIn("std::ofstream", body)
        for direct_swap in ("DeleteFileW", "MoveFileW", "fs::remove", "fs::rename"):
            self.assertNotIn(direct_swap, body)

    def test_metadata_m4a_album_art_uses_atomic_swap(self) -> None:
        source = (ROOT / "src" / "MetadataWriter.cpp").read_text(encoding="utf-8")
        body = source.split("bool MetadataWriter::writeM4AAlbumArt", 1)[1].split(
            "// WAV ID3 chunk writer", 1
        )[0]

        self.assertIn('atomicSwap(filePath, filePath + ".tmp")', body)
        self.assertIn("ReadBinaryFile(filePath, fileData)", body)
        self.assertIn("WriteBinaryFile(filePath + \".tmp\"", body)
        self.assertNotIn("std::ifstream", body)
        self.assertNotIn("std::ofstream", body)
        self.assertIn("ReadBinaryFile(filePath, fileData)", body)
        self.assertIn("WriteBinaryFile(filePath + \".tmp\"", body)
        self.assertNotIn("std::ifstream", body)
        self.assertNotIn("std::ofstream", body)
        for direct_swap in ("DeleteFileW", "MoveFileW", "fs::remove", "fs::rename"):
            self.assertNotIn(direct_swap, body)

    def test_metadata_wav_id3_album_art_uses_atomic_swap(self) -> None:
        source = (ROOT / "src" / "MetadataWriter.cpp").read_text(encoding="utf-8")
        body = source.split("bool MetadataWriter::writeWAVID3AlbumArt", 1)[1].split(
            "bool MetadataWriter::writeOGGAlbumArt", 1
        )[0]

        self.assertIn('atomicSwap(filePath, filePath + ".tmp")', body)
        self.assertIn("ReadBinaryFile(filePath, fileData)", body)
        self.assertIn("WriteBinaryFile(filePath + \".tmp\"", body)
        self.assertNotIn("std::ifstream", body)
        self.assertNotIn("std::ofstream", body)
        for direct_swap in ("DeleteFileW", "MoveFileW", "fs::remove", "fs::rename"):
            self.assertNotIn(direct_swap, body)

    def test_metadata_field_writer_streams_use_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "MetadataWriter.cpp").read_text(encoding="utf-8")
        boundaries = (
            ("writeID3v2Fields", "writeWAVFields"),
            ("writeWAVFields", "writeFLACFields"),
            ("writeFLACFields", None),
        )
        for start, end in boundaries:
            body = source.split(f"bool MetadataWriter::{start}", 1)[1]
            if end:
                body = body.split(f"bool MetadataWriter::{end}", 1)[0]
            self.assertIn("ReadBinaryFile(filePath, fileData)", body)
            self.assertIn("WriteBinaryFile(tempPath", body)
            self.assertNotIn("std::ifstream", body)
            self.assertNotIn("std::ofstream", body)

    def test_debugger_output_uses_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        windows = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        self.assertIn("WriteDebuggerOutput", header)
        self.assertIn("Platform::WriteDebuggerOutput", main)
        self.assertIn("Platform::WriteDebuggerOutput", windows)
        self.assertNotIn("OutputDebugStringA", main)
        self.assertNotIn("OutputDebugStringA", windows)

    def test_message_box_calls_use_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(encoding="utf-8")
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        windows = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")

        self.assertEqual(4, main.count("Platform::ShowMessageBox("))
        self.assertEqual(10, windows.count("Platform::ShowMessageBox("))
        self.assertNotIn("MessageBoxW", main)
        self.assertNotIn("MessageBoxW", windows)

        self.assertRegex(
            header,
            r"int ShowMessageBox\(void\* ownerWindow,\s*"
            r"const wchar_t\* message,\s*"
            r"const wchar_t\* title,\s*"
            r"unsigned int flags\);",
        )

        win32_body = win32.split("int ShowMessageBox", 1)[1].split("bool LaunchProcess", 1)[0]
        self.assertNotIn("std::wstring", win32_body)
        self.assertEqual(1, win32_body.count("MessageBoxW("))
        self.assertRegex(
            win32_body,
            r"return MessageBoxW\(static_cast<HWND>\(ownerWindow\),\s*"
            r"message,\s*title,\s*static_cast<UINT>\(flags\)\);",
        )

        macos_body = macos.split("int ShowMessageBox", 1)[1].split("bool LaunchProcess", 1)[0]
        self.assertIn("const wchar_t* message", macos_body)
        self.assertIn("const wchar_t* title", macos_body)
        self.assertIn(
            "message ? std::wstring_view(message) : std::wstring_view{}",
            macos_body,
        )
        self.assertIn(
            "title ? std::wstring_view(title) : std::wstring_view{}",
            macos_body,
        )
        self.assertRegex(
            macos_body,
            r"if\s*\(\(flags\s*&\s*kMbIconWarning\)\s*==\s*kMbIconWarning\)\s*\{\s*"
            r"alert\.alertStyle\s*=\s*NSAlertStyleWarning;\s*\}\s*"
            r"else if\s*\(\(flags\s*&\s*kMbIconError\)\s*==\s*kMbIconError\)\s*\{\s*"
            r"alert\.alertStyle\s*=\s*NSAlertStyleCritical;\s*\}\s*"
            r"else\s*\{\s*alert\.alertStyle\s*=\s*NSAlertStyleInformational;\s*\}",
        )
        self.assertIn("NSAlertFirstButtonReturn", macos_body)
        self.assertRegex(
            macos_body,
            r"return\s+response\s*==\s*NSAlertFirstButtonReturn\s*\?\s*1\s*:\s*0\s*;",
        )

    def test_main_unique_drag_name_preserves_exclusive_probe_semantics(self) -> None:
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static std::string MakeUniqueDragName", 1)[1].split(
            "static void TrackTempDragFile", 1
        )[0]
        pal_body = win32.split("bool CanOpenExclusiveWritePath", 1)[1].split(
            "namespace {", 1
        )[0]

        self.assertIn("Platform::CanOpenExclusiveWritePath(dirUtf8 + candidateName)", body)
        self.assertIn("suffix++;", body)
        self.assertIn("CreateFileW", pal_body)
        self.assertIn("GENERIC_WRITE", pal_body)
        self.assertIn("OPEN_EXISTING", pal_body)
        self.assertIn("CloseHandle(hFile)", pal_body)
        self.assertIn("const DWORD err = GetLastError()", pal_body)
        self.assertIn(
            "return err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED;",
            pal_body,
        )

    def test_main_drag_window_actions_use_system_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertGreaterEqual(source.count("Platform::WindowShow("), 3)
        self.assertIn("Platform::WindowSetForeground(dropTargetWnd)", source)
        self.assertNotIn("ShowWindow(", source)
        self.assertNotIn("SetForegroundWindow(", source)

    def test_main_process_detection_uses_process_query_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "platform/process_query.h"', source)
        self.assertGreaterEqual(source.count("Platform::FindProcessIdsByImageName("), 2)
        self.assertIn("Platform::ReadProcessImagePath(", source)
        self.assertNotIn("#include <tlhelp32.h>", source)
        self.assertNotIn("CreateToolhelp32Snapshot", source)
        self.assertNotIn("Process32FirstW", source)
        self.assertNotIn("Process32NextW", source)
        self.assertNotIn("QueryFullProcessImageNameW", source)

    def test_main_reaper_install_paths_use_filesystem_and_text_pals(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static bool WideFileExists", 1)[1].split(
            "static bool IsNonBlockingReaperInstallFailure", 1
        )[0]
        self.assertIn("Platform::FileSystem::Exists", body)
        self.assertIn("Platform::FileSystem::CreateDirectories", body)
        self.assertIn("Platform::TextEncoding::WideToUtf8Permissive", body)
        self.assertNotIn("GetFileAttributesW", body)
        self.assertNotIn("CreateDirectoryW", body)
        self.assertNotIn("WideCharToMultiByte", body)

    def test_atomic_replace_归属文件系统能力(self) -> None:
        header_path = ROOT / "src" / "platform" / "filesystem.h"
        win32_source_path = ROOT / "src" / "platform" / "filesystem_win32.cpp"
        std_source_path = ROOT / "src" / "platform" / "filesystem_std.cpp"
        self.assertTrue(header_path.is_file(), "src/platform 缺少 filesystem.h")
        self.assertTrue(win32_source_path.is_file(), "src/platform 缺少 filesystem_win32.cpp")
        self.assertTrue(std_source_path.is_file(), "src/platform 缺少 filesystem_std.cpp")

        header = header_path.read_text(encoding="utf-8")
        win32_source = win32_source_path.read_text(encoding="utf-8")
        std_source = std_source_path.read_text(encoding="utf-8")
        compatibility_header = (ROOT / "src" / "platform" / "platform.h").read_text(
            encoding="utf-8"
        )
        metadata_source = (ROOT / "src" / "MetadataWriter.cpp").read_text(encoding="utf-8")

        self.assertIn("namespace Platform::FileSystem", header)
        self.assertIn("bool AtomicFileReplace(", header)
        self.assertIn("MoveFileExW", win32_source)
        self.assertIn("std::filesystem::rename", std_source)
        self.assertNotIn("AtomicFileReplace", compatibility_header)
        self.assertIn('#include "platform/filesystem.h"', metadata_source)
        self.assertIn("Platform::FileSystem::AtomicFileReplace", metadata_source)

    def test_image_decode_归属专用能力(self) -> None:
        header_path = ROOT / "src" / "platform" / "image_codec.h"
        win32_path = ROOT / "src" / "platform" / "image_codec_win32.cpp"
        macos_path = ROOT / "src" / "platform" / "image_codec_macos.mm"
        self.assertTrue(header_path.is_file(), "src/platform 缺少 image_codec.h")
        self.assertTrue(win32_path.is_file(), "src/platform 缺少 image_codec_win32.cpp")
        self.assertTrue(macos_path.is_file(), "src/platform 缺少 image_codec_macos.mm")

        header = header_path.read_text(encoding="utf-8")
        win32_source = win32_path.read_text(encoding="utf-8")
        macos_source = macos_path.read_text(encoding="utf-8")
        legacy_win32 = (ROOT / "src" / "platform" / "platform_win32.cpp").read_text(
            encoding="utf-8"
        )
        legacy_macos = (ROOT / "src" / "platform" / "platform_macos.mm").read_text(
            encoding="utf-8"
        )
        cover_source = (ROOT / "src" / "CoverModule.cpp").read_text(encoding="utf-8")

        self.assertIn("namespace Platform::ImageCodec", header)
        self.assertIn("struct DecodedImage", header)
        self.assertIn("DecodedImage DecodeImageFromMemory", win32_source)
        self.assertIn("DecodedImage DecodeImageFromMemory", macos_source)
        self.assertNotIn("DecodedImage DecodeImageFromMemory", legacy_win32)
        self.assertNotIn("DecodedImage DecodeImageFromMemory", legacy_macos)
        self.assertIn('#include "platform/image_codec.h"', cover_source)
        self.assertIn("Platform::ImageCodec::DecodeImageFromMemory", cover_source)

    def test_image_encode_归属专用能力(self) -> None:
        win32_source = (ROOT / "src" / "platform" / "image_codec_win32.cpp").read_text(
            encoding="utf-8"
        )
        macos_source = (ROOT / "src" / "platform" / "image_codec_macos.mm").read_text(
            encoding="utf-8"
        )
        legacy_sources = "\n".join(
            (ROOT / "src" / "platform" / name).read_text(encoding="utf-8")
            for name in ("platform_win32.cpp", "platform_macos.mm", "platform.h")
        )
        cover_source = (ROOT / "src" / "CoverModule.cpp").read_text(encoding="utf-8")
        stub_source = (ROOT / "tests" / "test_cover_runtime_stubs.cpp").read_text(
            encoding="utf-8"
        )

        definition = "std::vector<uint8_t> EncodeRgbaToJpeg"
        self.assertIn(definition, win32_source)
        self.assertIn(definition, macos_source)
        self.assertNotIn("EncodeRgbaToJpeg", legacy_sources)
        self.assertIn("Platform::ImageCodec::EncodeRgbaToJpeg", cover_source)
        self.assertNotIn('#include "platform/platform.h"', cover_source)
        self.assertNotIn('#include "src/platform/platform.h"', stub_source)

    def test_utils_exe_dir_delegates_to_pal_on_windows(self) -> None:
        source = (ROOT / "src" / "Utils.cpp").read_text(encoding="utf-8")
        body = source.split("std::string GetExeDirUtf8()", 1)[1].split(
            "std::wstring GetDefaultDbDirWide", 1
        )[0]
        self.assertIn("Platform::GetExeDir()", body)
        self.assertNotIn("GetModuleFileNameW", body)

    def test_ai_engine_location_queries_delegate_to_pal(self) -> None:
        source = (ROOT / "src" / "AIEngine.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "platform/executable_path.h"', source)
        model_dir = source.split("std::string GetDefaultModelDir()", 1)[1].split(
            "// UTF-8", 1
        )[0]
        self.assertIn("Platform::GetExecutablePath()", model_dir)
        self.assertIn("Platform::ExecutablePathStatus::Success", model_dir)
        self.assertIn('return "./models";', model_dir)
        self.assertNotIn("GetModuleFileNameW", model_dir)

        constructor = source.split("AIEngine::AIEngine()", 1)[1].split(
            "AIEngine::~AIEngine()", 1
        )[0]
        self.assertIn("Platform::FileSystem::TemporaryDirectory()", constructor)
        self.assertIn('"Airs_AI"', constructor)
        self.assertIn('m_config.tempDir = "/tmp/Airs_AI/";', constructor)
        self.assertNotIn("GetTempPathW", constructor)

    def test_dead_temp_file_path_helper_is_retired(self) -> None:
        source = (ROOT / "src" / "Utils.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src" / "Utils.h").read_text(encoding="utf-8")

        self.assertNotIn("std::string GetTempFilePath(", source)
        self.assertNotIn("GetTempPathW", source)
        self.assertNotIn("GetTempFilePath", header)

        occurrences = []
        source_suffixes = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}
        for path in (ROOT / "src").rglob("*"):
            if path.is_file() and path.suffix.lower() in source_suffixes:
                text = path.read_text(encoding="utf-8", errors="replace")
                if "GetTempFilePath" in text:
                    occurrences.append(path.relative_to(ROOT).as_posix())
        self.assertEqual([], sorted(occurrences))

    def test_dead_whitelist_download_helper_is_retired(self) -> None:
        source = (ROOT / "src" / "Utils.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src" / "Utils.h").read_text(encoding="utf-8")

        self.assertNotIn("std::string DownloadWhitelistFromCloud(", source)
        self.assertNotIn("DownloadWhitelistFromCloud", header)

        occurrences = []
        source_suffixes = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}
        for path in (ROOT / "src").rglob("*"):
            if path.is_file() and path.suffix.lower() in source_suffixes:
                text = path.read_text(encoding="utf-8", errors="replace")
                if "DownloadWhitelistFromCloud" in text:
                    occurrences.append(path.relative_to(ROOT).as_posix())
        self.assertEqual([], sorted(occurrences))

    def test_dead_activation_reporting_helper_is_retired(self) -> None:
        source = (ROOT / "src" / "Utils.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src" / "Utils.h").read_text(encoding="utf-8")

        self.assertNotIn("void SendActivationToDingTalkAsync(", source)
        self.assertNotIn("SendActivationToDingTalkAsync", header)

        occurrences = []
        source_suffixes = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}
        for path in (ROOT / "src").rglob("*"):
            if path.is_file() and path.suffix.lower() in source_suffixes:
                text = path.read_text(encoding="utf-8", errors="replace")
                if "SendActivationToDingTalkAsync" in text:
                    occurrences.append(path.relative_to(ROOT).as_posix())
        self.assertEqual([], sorted(occurrences))

    def test_utils_dead_secure_token_and_wininet_dependency_are_retired(self) -> None:
        source = (ROOT / "src" / "Utils.cpp").read_text(encoding="utf-8")
        ai_source = (ROOT / "src" / "AIEngine.cpp").read_text(encoding="utf-8")
        windows_main = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        cuevi_feedback = (ROOT / "src" / "CueviFeedbackClient.cpp").read_text(encoding="utf-8")

        self.assertNotIn("GetSecureToken", source)
        self.assertNotIn("#include <wininet.h>", source)
        self.assertNotIn('#pragma comment(lib, "wininet.lib")', source)
        for token in (
            "HINTERNET", "InternetOpen", "InternetConnect", "InternetCloseHandle",
            "InternetReadFile", "HttpOpenRequest", "HttpSendRequest", "INTERNET_",
        ):
            self.assertNotIn(token, source)

        self.assertNotIn("#include <wininet.h>", windows_main)
        self.assertNotIn('#pragma comment(lib, "Wininet.lib")', windows_main)
        self.assertNotIn("HINTERNET", windows_main)
        self.assertNotIn("InternetOpenA(", windows_main)
        self.assertIn('#include "platform/system.h"', ai_source)
        self.assertEqual(ai_source.count("Platform::CloseInternetHandle("), 13)
        self.assertNotIn("InternetCloseHandle(", ai_source)
        self.assertNotIn("HINTERNET", ai_source)
        self.assertNotIn("InternetOpenW(", ai_source)
        self.assertNotIn("Platform::CloseInternetHandle", windows_main)
        self.assertIn("Platform::CloseInternetHandle", cuevi_feedback)

    def test_utils_utf_conversion_delegates_to_private_pal(self) -> None:
        source = (ROOT / "src" / "Utils.cpp").read_text(encoding="utf-8")
        utf8_body = source.split("std::wstring Utf8ToWide", 1)[1].split(
            "std::string WideToUtf8", 1
        )[0]
        wide_body = source.split("std::string WideToUtf8", 1)[1].split(
            "fs::path Utf8ToPath", 1
        )[0]
        self.assertIn("Platform::TextEncoding::Utf8ToWide(str)", utf8_body)
        self.assertIn("Platform::TextEncoding::WideToUtf8(wstr)", wide_body)
        self.assertNotIn("MultiByteToWideChar", utf8_body)
        self.assertNotIn("WideCharToMultiByte", wide_body)

    def test_win32_pal_reuses_private_text_encoding(self) -> None:
        helper_path = ROOT / "src" / "platform" / "text_encoding.inl"
        self.assertTrue(helper_path.is_file())
        helper = helper_path.read_text(encoding="utf-8")
        self.assertIn("namespace Platform::TextEncoding", helper)
        self.assertIn("Utf8ToWide", helper)
        self.assertIn("WideToUtf8", helper)

        source = "\n".join(
            (ROOT / "src" / "platform" / name).read_text(encoding="utf-8")
            for name in ("platform_win32.cpp", "system_win32.cpp")
        )
        self.assertIn("TextEncoding::Utf8ToWide", source)
        self.assertIn("TextEncoding::WideToUtf8", source)
        self.assertNotIn("Utf8ToWideLocal", source)
        self.assertNotIn("WideToUtf8Local", source)

    def test_utils_database_paths_delegate_to_private_pal(self) -> None:
        source = (ROOT / "src" / "Utils.cpp").read_text(encoding="utf-8")
        default_body = source.split("std::wstring GetDefaultDbDirWide()", 1)[1].split(
            "std::string GetDefaultDbDirUtf8", 1
        )[0]
        legacy_body = source.split("std::string GetLegacyDefaultDbPathUtf8()", 1)[1].split(
            "bool IsLegacyAuroOwnedDatabasePathUtf8", 1
        )[0]
        self.assertIn("Platform::DatabasePaths::GetDefaultDbDirWide()", default_body)
        self.assertIn("Platform::DatabasePaths::GetLegacyDefaultDbPathWide()", legacy_body)
        for body in (default_body, legacy_body):
            self.assertNotIn("SHGetFolderPathW", body)
            self.assertNotIn("CreateDirectoryW", body)

        helper_path = ROOT / "src" / "platform" / "database_paths.inl"
        self.assertTrue(helper_path.is_file())
        helper = helper_path.read_text(encoding="utf-8")
        self.assertIn("namespace Platform::DatabasePaths", helper)
        self.assertIn("GetDefaultDbDirWide", helper)
        self.assertIn("GetLegacyDefaultDbPathWide", helper)

    def test_database_hidden_attributes_use_filesystem_pal(self) -> None:
        database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        filesystem = (ROOT / "src" / "platform" / "filesystem.h").read_text(
            encoding="utf-8"
        )
        win32 = (ROOT / "src" / "platform" / "filesystem_win32.cpp").read_text(
            encoding="utf-8"
        )
        body = database.split("static bool isHiddenPath", 1)[1].split(
            "static bool hasAudioExtension", 1
        )[0]

        self.assertIn("struct PathAttributes", filesystem)
        self.assertIn("PathAttributes QueryPathAttributes(", filesystem)
        self.assertIn(
            "Platform::FileSystem::QueryPathAttributes(PathToUtf8(p))", body
        )
        for flag in ("attributes.hidden", "attributes.system", "attributes.temporary"):
            self.assertIn(flag, body)
        self.assertNotIn("GetFileAttributesW", body)
        self.assertIn("GetFileAttributesW", win32)

    def test_utils_file_times_delegate_to_private_pal(self) -> None:
        source = (ROOT / "src" / "Utils.cpp").read_text(encoding="utf-8")
        body = source.split("bool GetFileTimesUtf8", 1)[1].split(
            "std::string BuildDatabasePackageMainPathUtf8", 1
        )[0]
        self.assertIn(
            "Platform::FileTimes::GetFileTimesWide(wFilePath, outCreationUnix, outModifiedUnix)",
            body,
        )
        for native_call in ("CreateFileW(", "GetFileTime(", "CloseHandle("):
            self.assertNotIn(native_call, body)

        helper_path = ROOT / "src" / "platform" / "file_times.inl"
        self.assertTrue(helper_path.is_file())
        helper = helper_path.read_text(encoding="utf-8")
        self.assertIn("namespace Platform::FileTimes", helper)
        for native_call in ("CreateFileW", "GetFileTime", "CloseHandle"):
            self.assertIn(native_call, helper)

    def test_utils_filesystem_io_uses_pal(self) -> None:
        source = (ROOT / "src" / "Utils.cpp").read_text(encoding="utf-8")
        exists_body = source.split("bool PathExistsUtf8", 1)[1].split(
            "std::string GetExeDirUtf8", 1
        )[0]
        load_body = source.split("LoadKeyValueFile", 1)[1].split(
            "void SaveKeyValueFile", 1
        )[0]
        save_body = source.split("void SaveKeyValueFile", 1)[1].split(
            "bool ParseBool", 1
        )[0]
        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::Exists(pathUtf8)", exists_body)
        self.assertIn("Platform::FileSystem::ReadFileText(pathUtf8, contents)", load_body)
        self.assertIn("Platform::FileSystem::WriteFileText(pathUtf8, out.str())", save_body)
        for direct_io in ("fs::exists", "std::ifstream", "std::ofstream"):
            self.assertNotIn(direct_io, source)

    def test_transfer_destination_probe_uses_path_status_pal(self) -> None:
        header = (ROOT / "src" / "TransferEngine.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "TransferEngine.cpp").read_text(encoding="utf-8")
        filesystem = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")

        self.assertIn("enum class PathStatus", filesystem)
        self.assertIn("PathStatus ProbePath(", filesystem)
        self.assertIn("Platform::FileSystem::ProbePath", source)
        self.assertNotIn("fs::exists", header + source)
        self.assertNotIn("std::filesystem::exists", header + source)
        self.assertNotIn("resolveExistingDestinationPath(", header.split("inline", 1)[-1])

    def test_waveform_reader_uses_opaque_incremental_stream_pal(self) -> None:
        filesystem = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WaveformExtractor.cpp").read_text(encoding="utf-8")

        for declaration in (
            "class StreamReader",
            "bool Open(const std::string& pathUtf8, bool binary);",
            "size_t Read(void* destination, size_t bytes);",
            "bool Seek(int64_t offset, StreamSeekOrigin origin);",
            "int64_t Tell() const;",
            "bool Good() const;",
            "void Clear();",
            "struct Impl;",
        ):
            self.assertIn(declaration, filesystem)
        self.assertIn("Platform::FileSystem::StreamReader", source)
        self.assertNotIn("std::ifstream", source)
        self.assertNotIn("#include <fstream>", source)
        self.assertNotIn("OpenBinaryInput(const std::string& filePath, std::ifstream&", source)

    def test_waveform_extractor_path_text_uses_permissive_pal(self) -> None:
        source = (ROOT / "src" / "WaveformExtractor.cpp").read_text(encoding="utf-8")

        self.assertNotIn('#include "platform/text_encoding.h"', source)
        self.assertNotIn("InitDecoderForPath", source)
        for native_text in (
            "windows.h",
            "MultiByteToWideChar",
            "CP_UTF8",
            "static std::wstring Utf8ToWide",
        ):
            self.assertNotIn(native_text, source)

        self.assertNotIn("ma_decoder_", source)

    def test_media_foundation_path_uses_permissive_text_encoding_pal(self) -> None:
        header = (
            ROOT / "src" / "MediaFoundationAudioDecoder.h"
        ).read_text(encoding="utf-8")
        win32 = (
            ROOT / "src/platform/media_foundation_audio_win32.cpp"
        ).read_text(encoding="utf-8")
        self.assertNotIn('#include "platform/text_encoding.h"', header)
        self.assertIn('#include "text_encoding.h"', win32)
        self.assertIn(
            "ToMediaFoundationPath("
            "Platform::TextEncoding::Utf8ToWidePermissive(path))",
            win32,
        )
        self.assertNotIn("inline std::wstring Utf8ToWide", header)
        self.assertNotIn("MultiByteToWideChar", header)
        self.assertNotIn("CP_UTF8", header)
        self.assertIn("inline std::wstring ToMediaFoundationPath", header)
        self.assertIn("ClassifyFailure", header)
        for preserved in (
            "#include <windows.h>", "#include <mfapi.h>",
            "#include <mfidl.h>", "#include <mfreadwrite.h>",
            "#include <propvarutil.h>", "#include <wrl/client.h>",
            "MFStartup(MF_VERSION, MFSTARTUP_LITE)",
            "CoInitializeEx(nullptr, COINIT_MULTITHREADED)",
            "if (uninit) CoUninitialize();", "MFCreateSourceReaderFromURL",
        ):
            self.assertIn(preserved, win32)

        cmake = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(
            encoding="utf-8"
        )
        wma_block = cmake.split("add_executable(TestWmaDecodeSupport", 1)[1].split(
            "add_test(NAME TestWmaDecodeSupport", 1
        )[0]
        self.assertIn("if(WIN32)", wma_block)
        self.assertIn("target_sources(TestWmaDecodeSupport PRIVATE", wma_block)
        self.assertIn("src/platform/text_encoding_win32.cpp", wma_block)
        self.assertNotIn("src/platform/text_encoding_std.cpp", wma_block)

    def test_media_foundation_header_owns_com_and_mf_lifetimes(self) -> None:
        header = (ROOT / "src" / "MediaFoundationAudioDecoder.h").read_text(
            encoding="utf-8"
        )
        win32 = (
            ROOT / "src/platform/media_foundation_audio_win32.cpp"
        ).read_text(encoding="utf-8")
        unsupported = (
            ROOT / "src/platform/media_foundation_audio_unsupported.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("#include <cctype>", header)
        for native_include in (
            "windows.h", "mfapi.h", "mfidl.h", "mfreadwrite.h",
            "propvarutil.h", "wrl/client.h",
        ):
            self.assertNotIn(f"#include <{native_include}>", header)
            self.assertEqual(1, win32.count(f"#include <{native_include}>"))
        for native_token in (
            "HRESULT", "DWORD", "LONGLONG", "BYTE", "GUID", "PROPVARIANT",
            "IMFSourceReader", "IMFMediaBuffer", "Microsoft::WRL::ComPtr",
            "CoInitializeEx", "CoUninitialize", "MFStartup",
        ):
            self.assertNotIn(native_token, header)

        self.assertNotIn("MFStartup(", header)
        self.assertNotIn("CoInitializeEx(", header)
        self.assertNotIn("CoUninitialize()", header)
        self.assertIn("bool EnsureStarted", win32)
        startup = win32.split("bool EnsureStarted", 1)[1].split(
            "ComApartment::ComApartment", 1
        )[0]
        self.assertIn("static std::once_flag once;", startup)
        self.assertIn("static HRESULT startupHr = E_FAIL;", startup)
        self.assertIn("std::call_once(once", startup)
        self.assertEqual(1, startup.count("MFStartup(MF_VERSION, MFSTARTUP_LITE)"))
        self.assertIn("if (FAILED(startupHr))", startup)
        self.assertIn("HrToString(startupHr)", startup)
        self.assertNotIn("MFShutdown", header)

        apartment = win32.split("ComApartment::ComApartment", 1)[1].split(
            "} // namespace detail", 1
        )[0]
        self.assertIn("CoInitializeEx(nullptr, COINIT_MULTITHREADED)", apartment)
        self.assertIn("uninit = SUCCEEDED(hr);", apartment)
        self.assertNotIn("hr == S_OK", apartment)
        self.assertIn("if (hr == RPC_E_CHANGED_MODE)", apartment)
        self.assertIn("hr = S_OK;", apartment)
        self.assertIn("uninit = false;", apartment)
        self.assertEqual(1, apartment.count("CoUninitialize()"))
        self.assertIn("if (uninit) CoUninitialize();", apartment)

        self.assertNotIn("inline bool CreateReader", header)
        self.assertNotIn("inline bool GetCurrentAudioFormat", header)
        self.assertIn("bool CreateReader", win32)
        self.assertIn("bool GetCurrentAudioFormat", win32)
        create = win32.split("bool CreateReader", 1)[1].split(
            "bool GetCurrentAudioFormat", 1
        )[0]
        create_order = (
            "EnsureStarted", "MFCreateAttributes", "SetUINT32",
            "Utf8ToWidePermissive", "MFCreateSourceReaderFromURL",
            "MFCreateMediaType", "SetCurrentMediaType",
        )
        self.assertEqual(list(create_order), sorted(
            create_order, key=lambda token: create.index(token)
        ))
        self.assertIn("MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE", create)
        self.assertNotIn("hr = attrs->SetUINT32", create)
        self.assertIn("MFMediaType_Audio", create)
        self.assertIn("MFAudioFormat_Float", create)
        self.assertNotIn("hr = type->SetGUID", create)
        self.assertIn("Microsoft::WRL::ComPtr<IMFAttributes>", create)
        self.assertIn("Microsoft::WRL::ComPtr<IMFMediaType>", create)

        audio_format = win32.split("bool GetCurrentAudioFormat", 1)[1].split(
            "} // namespace detail", 1
        )[0]
        for reset in (
            "sampleRate = 0;", "channels = 0;", "subtype = GUID_NULL;",
            "bitsPerSample = 0;",
        ):
            self.assertIn(reset, audio_format)
        for attribute in (
            "MF_MT_AUDIO_SAMPLES_PER_SECOND", "MF_MT_AUDIO_NUM_CHANNELS",
            "MF_MT_AUDIO_BITS_PER_SAMPLE", "MF_MT_SUBTYPE",
        ):
            self.assertIn(attribute, audio_format)
        self.assertIn('"missing_audio_format"', audio_format)
        self.assertIn("type->GetGUID(MF_MT_SUBTYPE, &subtype);", audio_format)

        self.assertNotIn("inline bool AppendBufferAsFloat", header)
        self.assertIn("bool AppendBufferAsFloat", win32)
        append = win32.split("bool AppendBufferAsFloat", 1)[1].split(
            "} // namespace detail", 1
        )[0]
        self.assertLess(append.index("buffer->Lock"), append.index("MFAudioFormat_Float"))
        for conversion in (
            "MFAudioFormat_Float", "bitsPerSample == 16", "32768.0f",
            "bitsPerSample == 32", "2147483648.0", "unsupported_pcm_subtype",
        ):
            self.assertIn(conversion, append)
        self.assertEqual(2, append.count("buffer->Unlock()"))
        self.assertNotIn("FAILED(buffer->Unlock", append)

        self.assertIn("bool Probe(", header)
        self.assertNotIn("inline bool Probe(", header)
        self.assertIn("bool Probe(", win32)
        self.assertIn("bool Probe(", unsupported)
        probe = win32.split("bool Probe", 1)[1].split("bool Decode", 1)[0]
        for reset in (
            "sampleRate = 0;", "channels = 0;", "totalFrames = 0;",
            "durationSeconds = 0.0f;",
        ):
            self.assertIn(reset, probe)
        self.assertLess(
            probe.index("CanDecodeExtension"),
            probe.index("detail::ComApartment apartment"),
        )
        for ownership in (
            "detail::ComApartment apartment", "detail::CreateReader",
            "detail::GetCurrentAudioFormat", "PropVariantInit", "MF_PD_DURATION",
            "PropVariantClear", "missing_duration",
        ):
            self.assertIn(ownership, probe)
        unsupported_probe = unsupported.split("bool Probe", 1)[1].split(
            "bool Decode", 1
        )[0]
        for reset in (
            "sampleRate = 0;", "channels = 0;", "totalFrames = 0;",
            "durationSeconds = 0.0f;", "media_foundation_extension_not_supported",
            "media_foundation_unavailable",
        ):
            self.assertIn(reset, unsupported_probe)

        self.assertIn("bool Decode(", header)
        self.assertNotIn("inline bool Decode(", header)
        self.assertIn("bool Decode(", win32)
        self.assertIn("bool Decode(", unsupported)
        decode = win32.split("bool Decode", 1)[1]
        self.assertIn("out = {};", decode)
        self.assertIn("detail::ComApartment apartment", decode)
        self.assertIn("Probe(path, probeSr, probeCh, probeFrames", decode)
        self.assertIn("cancelFlag->load(std::memory_order_acquire)", decode)
        self.assertIn("reader->ReadSample", decode)
        self.assertIn("Microsoft::WRL::ComPtr<IMFSourceReader>", decode)
        self.assertIn("Microsoft::WRL::ComPtr<IMFSample>", decode)
        self.assertIn("Microsoft::WRL::ComPtr<IMFMediaBuffer>", decode)
        self.assertLess(decode.index("MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED"),
                        decode.index("MF_SOURCE_READERF_ENDOFSTREAM"))
        self.assertLess(decode.index("MF_SOURCE_READERF_ENDOFSTREAM"),
                        decode.index("if (!sample) continue"))
        self.assertIn("sample->ConvertToContiguousBuffer", decode)
        self.assertIn("detail::AppendBufferAsFloat", decode)
        self.assertIn("out.interleaved.size() / (size_t)out.channels", decode)
        self.assertIn('ClassifyFailure("decode_no_frames")', decode)
        unsupported_decode = unsupported.split("bool Decode", 1)[1]
        for preserved in (
            "out = {};", "media_foundation_extension_not_supported",
            "media_foundation_unavailable",
        ):
            self.assertIn(preserved, unsupported_decode)

    def test_media_foundation_header_consumer_and_link_topology(self) -> None:
        direct_consumers = (
            "src/AudioEngine.cpp", "src/WaveformExtractor.h",
            "src/WaveformExtractor.cpp", "tests/test_wma_decode_support.cpp",
        )
        for relative in direct_consumers:
            text = (ROOT / relative).read_text(encoding="utf-8")
            self.assertIn('#include "MediaFoundationAudioDecoder.h"', text)

        transitive_waveform_consumers = (
            "src/AsyncWorker.cpp", "src/AudioEngine.cpp", "src/BackfillWorkerPool.cpp",
            "src/Database.cpp", "src/main.cpp", "src/WaveformExtractor.cpp",
            "tests/test_mini_waveform_decode_perf.cpp",
            "tests/test_scan_waveform_cache_contract.cpp",
        )
        for relative in transitive_waveform_consumers:
            text = (ROOT / relative).read_text(encoding="utf-8")
            self.assertIn('#include "WaveformExtractor.h"', text)

        waveform_header = (ROOT / "src" / "WaveformExtractor.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("MediaFoundationAudio::DecodedAudioBuffer& outDecoded", waveform_header)
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        # Media Foundation belongs to the Aris application target only.  A
        # global link_libraries() call would leak those audio dependencies into
        # the independent Bootstrap target and violate its zero-audio boundary.
        self.assertNotRegex(
            cmake,
            re.compile(
                r"link_libraries\(\s*mfplat\s+mfreadwrite\s+mfuuid\s*\)",
                re.MULTILINE,
            ),
        )
        aris_link = cmake.split("target_link_libraries(HarkenSearch PRIVATE", 1)[1].split(
            ")", 1
        )[0]
        for library in ("ole32", "mfplat", "mfreadwrite", "mfuuid"):
            self.assertIn(library, aris_link)

        selected_source = "${AURO_MEDIA_FOUNDATION_AUDIO_SOURCE}"
        self.assertIn(
            "set(AURO_MEDIA_FOUNDATION_AUDIO_SOURCE "
            "src/platform/media_foundation_audio_win32.cpp)", cmake,
        )
        self.assertIn(
            "set(AURO_MEDIA_FOUNDATION_AUDIO_SOURCE "
            "src/platform/media_foundation_audio_unsupported.cpp)", cmake,
        )
        app_sources = cmake.split("set(APP_SOURCES", 1)[1].split(")", 1)[0]
        self.assertEqual(1, app_sources.count(selected_source))

        behavior_cmake = (
            ROOT / "cmake" / "harken_search_behavior_tests.cmake"
        ).read_text(encoding="utf-8")
        search_workspace_link = behavior_cmake.split(
            "target_link_libraries(TestSearchWorkspace PRIVATE", 1
        )[1].split(")", 1)[0]
        for library in ("mfplat", "mfreadwrite", "mfuuid"):
            self.assertIn(library, search_workspace_link)
        wma_block = behavior_cmake.split(
            "add_executable(TestWmaDecodeSupport", 1
        )[1].split("add_test(NAME TestWmaDecodeSupport", 1)[0]
        waveform_loop = behavior_cmake.split(
            "foreach(_auro_waveform_text_target", 1
        )[1].split("endforeach()", 1)[0]
        database_probe_integration = behavior_cmake.split(
            "add_executable(TestDatabaseAudioProbeIntegration", 1
        )[1].split(
            "set_property(TARGET TestDatabaseAudioProbeIntegration", 1
        )[0]
        self.assertEqual(1, wma_block.count(selected_source))
        self.assertEqual(1, waveform_loop.count(selected_source))
        self.assertEqual(1, database_probe_integration.count(selected_source))
        playlist_block = behavior_cmake.split("add_executable(TestPreviewPlaylistDatabase", 1)[1].split("add_test(", 1)[0]
        self.assertEqual(1, playlist_block.count(selected_source))
        for library in ("mfplat", "mfreadwrite", "mfuuid"):
            self.assertIn(library, playlist_block)
        self.assertEqual(4, behavior_cmake.count(selected_source))

        for library in ("mfplat", "mfreadwrite", "mfuuid"):
            self.assertIn(library, wma_block)
            self.assertIn(library, waveform_loop)

        for target_name in (
            "TestPlaylistPerformanceContract",
            "TestPerfOptimizations",
            "TestMiniEnvelopeBuild",
            "TestSimilarityQueryService",
        ):
            target_block = behavior_cmake.split(
                f"add_executable({target_name}", 1
            )[1].split(f"add_test(NAME {target_name}", 1)[0]
            for library in ("mfplat", "mfreadwrite", "mfuuid"):
                self.assertIn(library, target_block, target_name)

        header_test = behavior_cmake.split(
            "add_executable(TestMediaFoundationHeaderSelfContained", 1
        )[1].split("add_executable(TestAudioFileReader", 1)[0]
        self.assertIn(
            "tests/test_media_foundation_header_self_contained.cpp", header_test
        )
        self.assertIn(
            "add_test(NAME TestMediaFoundationHeaderSelfContained", header_test
        )
        self.assertNotIn(selected_source, header_test)
        self.assertNotIn("text_encoding", header_test)
        self.assertNotIn("mfplat", header_test)
        self.assertIn(
            'set_property(TARGET TestMediaFoundationHeaderSelfContained '
            'PROPERTY LINK_LIBRARIES "")',
            header_test,
        )

        header_test_source = (
            ROOT / "tests/test_media_foundation_header_self_contained.cpp"
        ).read_text(encoding="utf-8")
        self.assertTrue(
            header_test_source.startswith(
                '#include "MediaFoundationAudioDecoder.h"\n\n#include <'
            )
        )
        self.assertEqual(1, header_test_source.count('#include "'))
        self.assertNotIn("Probe(", header_test_source)
        self.assertNotIn("Decode(", header_test_source)

        regression = (
            ROOT / "tools/behavior_regression_suite.py"
        ).read_text(encoding="utf-8")
        platform_services = regression.split(
            '"platform-services",', 1
        )[1].split(")", 1)[0]
        self.assertEqual(
            1, platform_services.count("TestMediaFoundationHeaderSelfContained")
        )
        self.assertTrue(
            (ROOT / "src/platform/media_foundation_audio_win32.cpp").is_file()
        )
        self.assertTrue(
            (ROOT / "src/platform/media_foundation_audio_unsupported.cpp").is_file()
        )

    def test_metadata_reader_uses_stream_pal_for_binary_parsing(self) -> None:
        source = (ROOT / "src" / "MetadataReader.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("class MetadataFileReader", source)
        self.assertIn("Platform::FileSystem::StreamReader", source)
        self.assertIn("Platform::FileSystem::ProbePath", source)
        self.assertNotIn("std::ifstream", source)
        self.assertNotIn("#include <fstream>", source)
        self.assertNotIn("fs::exists", source)

    def test_metadata_reader_path_encoding_is_separate_from_tag_content(self) -> None:
        source = (ROOT / "src" / "MetadataReader.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "platform/text_encoding.h"', source)
        self.assertNotIn("static std::wstring Utf8ToWide", source)

        metadata_body = source.split(
            "bool MetadataReader::readMetadata", 1
        )[1].split("bool MetadataReader::readID3v1", 1)[0]
        album_body = source.split(
            "bool MetadataReader::readAlbumArt", 1
        )[1].split("bool MetadataReader::readFLACAlbumArt", 1)[0]
        self.assertEqual(
            1, metadata_body.count("TextEncoding::Utf8ToWidePermissive(filePath)")
        )
        self.assertEqual(
            1, metadata_body.count("if (!filePath.empty() && wpath.empty()) return false;")
        )
        self.assertEqual(
            1, metadata_body.count("TextEncoding::WideToUtf8Permissive(wext)")
        )
        self.assertNotIn("MultiByteToWideChar", metadata_body)
        self.assertNotIn("WideCharToMultiByte", metadata_body)
        self.assertEqual(
            2, album_body.count("TextEncoding::Utf8ToWidePermissive(filePath)")
        )
        self.assertEqual(
            1, album_body.count("if (!filePath.empty() && wpath.empty()) return false;")
        )
        self.assertEqual(
            1, album_body.count("TextEncoding::WideToUtf8Permissive(wext)")
        )
        self.assertNotIn("MultiByteToWideChar", album_body)
        self.assertNotIn("WideCharToMultiByte", album_body)
        sidecar_body = album_body.split(
            "// If embedded art is absent", 1
        )[1]
        self.assertIn("#ifdef _WIN32", sidecar_body)
        self.assertIn("fs::path p(filePath);", sidecar_body)
        self.assertIn("fs::path stemPath = p.stem();", sidecar_body)
        self.assertIn(
            'static const char* kSidecarSuffixes[] = { "", "_thumb", '
            '"_art", "_cover", "-thumb", "-art" };',
            sidecar_body,
        )
        self.assertIn(
            'for (const char* imgExt : { ".jpg", ".png", ".jpeg" })',
            sidecar_body,
        )
        self.assertLess(
            sidecar_body.index("for (const char* suffix"),
            sidecar_body.index("for (const char* imgExt"),
        )

        self.assertNotIn("Metadata::LegacyTagContent", metadata_body)
        self.assertNotIn("Metadata::LegacyTagContent", album_body)

    @staticmethod
    def _strip_cpp_comments(text: str) -> str:
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
        return re.sub(r"//[^\n]*", "", text)

    @classmethod
    def _function_body(cls, source: str, signature: str) -> str:
        code = cls._strip_cpp_comments(source)
        start = code.find(signature)
        if start < 0:
            raise AssertionError(f"missing function signature: {signature}")
        brace = code.find("{", start)
        if brace < 0:
            raise AssertionError(f"missing function body: {signature}")
        depth = 0
        quote = None
        escaped = False
        for index in range(brace, len(code)):
            char = code[index]
            if quote:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quote:
                    quote = None
                continue
            if char in ("'", '"'):
                quote = char
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return code[brace + 1:index]
        raise AssertionError(f"unbalanced function body: {signature}")

    @staticmethod
    def _parenthesized_block(source: str, marker: str) -> str:
        start = source.find(marker)
        opening = source.find("(", start)
        if start < 0 or opening < 0:
            raise AssertionError(f"missing parenthesized block: {marker}")
        depth = 0; quote = None; escaped = False
        for index in range(opening, len(source)):
            char = source[index]
            if quote:
                if escaped: escaped = False
                elif char == "\\": escaped = True
                elif char == quote: quote = None
            elif char in ("'", '"'): quote = char
            elif char == "(": depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0: return source[opening + 1:index]
        raise AssertionError(f"unbalanced parenthesized block: {marker}")

    def test_metadata_legacy_tag_content_api_backend_contract(self) -> None:
        header_path = ROOT / "src/platform/metadata_legacy_tag_content.h"
        win32_path = ROOT / "src/platform/metadata_legacy_tag_content_win32.cpp"
        std_path = ROOT / "src/platform/metadata_legacy_tag_content_std.cpp"
        cmake_path = ROOT / "CMakeLists.txt"
        behavior_path = ROOT / "cmake/harken_search_behavior_tests.cmake"
        for path in (header_path, win32_path, std_path, cmake_path, behavior_path):
            self.assertTrue(path.is_file(), f"missing required contract file: {path}")
        header = self._strip_cpp_comments(header_path.read_text(encoding="utf-8"))
        win32 = self._strip_cpp_comments(win32_path.read_text(encoding="utf-8"))
        standard = self._strip_cpp_comments(std_path.read_text(encoding="utf-8"))
        cmake = re.sub(r"(?m)#.*$", "", cmake_path.read_text(encoding="utf-8"))
        behavior = re.sub(r"(?m)#.*$", "", behavior_path.read_text(encoding="utf-8"))
        self.assertIn("std::string DecodeId3v2Encoding0(std::string_view bytes);", header)
        self.assertIn("std::string DecodeUtf16(const uint8_t* bytes, size_t size, Utf16ByteOrder order);", header)
        self.assertIn("enum class Utf16ByteOrder : uint8_t", header)
        for forbidden in ("windows.h", "DecodeStatus", "DecodeResult", "ConversionFailed", "CP_1252"):
            self.assertNotIn(forbidden, header)
        self.assertIn("constexpr UINT kCp1252 = 1252;", win32)
        encoding0 = self._function_body(win32, "std::string DecodeId3v2Encoding0")
        utf16 = self._function_body(win32, "std::string DecodeUtf16")
        self.assertEqual(2, len(re.findall(r"MultiByteToWideChar\s*\(", encoding0)))
        self.assertEqual(2, len(re.findall(r"WideCharToMultiByte\s*\(", encoding0)))
        self.assertEqual(0, len(re.findall(r"MultiByteToWideChar\s*\(", utf16)))
        self.assertEqual(2, len(re.findall(r"WideCharToMultiByte\s*\(", utf16)))
        self.assertEqual(6, len(re.findall(r"(?:MultiByteToWideChar|WideCharToMultiByte)\s*\(", win32)))
        self.assertRegex(encoding0, r"\bwlen\s*=\s*MultiByteToWideChar\b")
        self.assertRegex(encoding0, r"\bu8len\s*=\s*WideCharToMultiByte\b")
        self.assertRegex(utf16, r"\bn8\s*=\s*WideCharToMultiByte\b")
        for name in ("wlen", "u8len", "n8"):
            self.assertRegex(win32, rf"if\s*\(\s*{name}\s*<=\s*0\s*\)\s*return")
        self.assertRegex(encoding0, r"(?m)^\s*MultiByteToWideChar\([^;]+wide\.data\([^;]+\);\s*$")
        self.assertRegex(encoding0, r"(?m)^\s*WideCharToMultiByte\([^;]+out\.data\([^;]+\);\s*$")
        self.assertRegex(utf16, r"(?m)^\s*WideCharToMultiByte\([^;]+out\.data\([^;]+\);\s*$")
        self.assertIn("std::wstring wide((size_t)wlen, L'\\0');", encoding0)
        self.assertIn("std::string out((size_t)u8len, '\\0');", encoding0)
        self.assertIn("std::wstring wide(n / 2, L'\\0');", utf16)
        self.assertIn("std::string out((size_t)n8, '\\0');", utf16)
        standard0 = self._function_body(standard, "std::string DecodeId3v2Encoding0")
        standard16 = self._function_body(standard, "std::string DecodeUtf16")
        self.assertIn("for (unsigned char c : b)", standard0)
        self.assertRegex(standard16, r"std::wstring\s+wide\s*\(\s*n\s*/\s*2")
        self.assertRegex(standard16, r"wide\[i\]\s*=\s*\(wchar_t\)")
        self.assertIn("wide.find(L'\\0')", standard16)
        self.assertIn("out.reserve(wide.size()*3)", standard16)
        self.assertNotIn("0xD800", standard16)
        for pattern in (r"c\s*<\s*0x80", r"0xC0\s*\|\s*\(c\s*>>\s*6\)", r"0x80\s*\|\s*\(c\s*&\s*0x3F\)"):
            self.assertRegex(standard0, pattern)
        for pattern in (r"\(a\s*<<\s*8\)\s*\|\s*c", r"\(c\s*<<\s*8\)\s*\|\s*a"):
            self.assertRegex(standard16, pattern)
        for pattern in (r"c\s*<\s*0x80", r"c\s*<\s*0x800", r"0xE0\s*\|\s*\(c\s*>>\s*12\)", r"0x80\s*\|\s*\(\(c\s*>>\s*6\)&\s*0x3F\)", r"0x80\s*\|\s*\(c\s*&\s*0x3F\)"):
            self.assertRegex(standard16, pattern)
        selected = "AURO_METADATA_LEGACY_TAG_CONTENT_SOURCE"
        token_positions = [m.start() for m in re.finditer(r"set\s*\(\s*" + selected, cmake)]
        self.assertEqual(2, len(token_positions))
        if_blocks = []
        for match in re.finditer(r"(?m)^\s*if\s*\(\s*WIN32\s*\)", cmake):
            end = cmake.find("endif", match.end())
            if end >= 0 and match.start() < token_positions[0] < end:
                if_blocks.append(cmake[match.start():end])
        self.assertEqual(1, len(if_blocks))
        selection = re.search(r"if\s*\(\s*WIN32\s*\)(.*?)else\s*\(\s*\)(.*)", if_blocks[0], re.DOTALL)
        self.assertIsNotNone(selection)
        win_selection, std_selection = selection.groups()
        self.assertEqual(1, len(re.findall(r"set\s*\(\s*" + selected, win_selection)))
        self.assertEqual(1, len(re.findall(r"set\s*\(\s*" + selected, std_selection)))
        self.assertIn("metadata_legacy_tag_content_win32.cpp", win_selection)
        self.assertIn("metadata_legacy_tag_content_std.cpp", std_selection)
        target = self._parenthesized_block(behavior, "add_executable(TestMetadataLegacyTagContent")
        self.assertEqual(1, target.count("tests/test_metadata_legacy_tag_content.cpp"))
        self.assertEqual(1, target.count("${" + selected + "}"))
        self.assertNotRegex(target, r"metadata_legacy_tag_content_(?:win32|std)\.cpp")
        add_test = self._parenthesized_block(behavior, "add_test(NAME TestMetadataLegacyTagContent")
        self.assertEqual(1, add_test.count("NAME TestMetadataLegacyTagContent"))
        self.assertEqual(1, add_test.count("COMMAND TestMetadataLegacyTagContent"))

    def test_metadata_reader_uses_legacy_tag_content_boundary(self) -> None:
        source = self._strip_cpp_comments((ROOT / "src/MetadataReader.cpp").read_text(encoding="utf-8")); parser = source.split("bool MetadataReader::readID3v2", 1)[1].split("bool MetadataReader::readBWF", 1)[0]
        dispatch = parser.split("std::string content;", 1)[1].split("content = trimString(content);", 1)[0]
        self.assertIn('#include "platform/metadata_legacy_tag_content.h"', source); self.assertEqual(1, parser.count("Platform::Metadata::LegacyTagContent::DecodeId3v2Encoding0(")); self.assertEqual(1, parser.count("Platform::Metadata::LegacyTagContent::DecodeUtf16("))
        for forbidden in ("windows.h", "MultiByteToWideChar", "WideCharToMultiByte", "CP_1252", "Latin1ToUtf8", "Utf16ToUtf8"): self.assertNotIn(forbidden, source)
        self.assertNotRegex(dispatch, r"\breturn\b")
        for required in ("if (encoding == 3)", "else if (encoding == 0)", "else if (encoding == 1 || encoding == 2)", "content = Platform::Metadata::LegacyTagContent::DecodeId3v2Encoding0(", "content = Platform::Metadata::LegacyTagContent::DecodeUtf16("): self.assertIn(required, dispatch)
        self.assertRegex(dispatch, r"auto\s+order\s*=\s*encoding\s*==\s*2\s*\?\s*Platform::Metadata::LegacyTagContent::Utf16ByteOrder::BigEndian\s*:\s*Platform::Metadata::LegacyTagContent::Utf16ByteOrder::LittleEndian"); self.assertRegex(dispatch, r"if\s*\([^{}]*0xFF[^{}]*0xFE[^{}]*\)\s*\{[^{}]*LittleEndian[^{}]*textStart\s*\+=\s*2[^{}]*textLen\s*-=\s*2[^{}]*\}"); self.assertRegex(dispatch, r"else\s+if\s*\([^{}]*0xFE[^{}]*0xFF[^{}]*\)\s*\{[^{}]*BigEndian[^{}]*textStart\s*\+=\s*2[^{}]*textLen\s*-=\s*2[^{}]*\}")
        self.assertRegex(dispatch, r"DecodeUtf16\(\s*tagData\.data\(\)\s*\+\s*textStart\s*,\s*textLen\s*,\s*order\s*\)"); self.assertIn("content = trimString(content);", parser); self.assertIn("std::string id(frameId)", parser)
        self.assertEqual([], [f for f in boundary.scan_tree(ROOT) if f.path == "src/MetadataReader.cpp"]); ledger = json.loads((ROOT / "docs/plans/refactor/stage-6-platform-migration-ledger.json").read_text(encoding="utf-8")); self.assertEqual([], [e for e in ledger["findings"] if e["path"] == "src/MetadataReader.cpp"])
        cmake = re.sub(r"(?m)#.*$", "", (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")); app = cmake.split("set(APP_SOURCES", 1)[1].split(")", 1)[0]; selected = "${AURO_METADATA_LEGACY_TAG_CONTENT_SOURCE}"
        self.assertEqual(1, app.count(selected))
        behavior = re.sub(r"(?m)#.*$", "", (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")); targets = ("TestMetadataReaderPathEncoding", "TestMetadataReaderContentEncoding", "TestStage6PlaybackLoadSeekProbe", "TestStage6ScanTerminalThroughputProbe", "TestSearchSemanticMatrix", "TestDbSearchFacets", "TestTrigramE2E", "TestFuzzySearchPrd", "TestBackfillRecoveryWorker", "TestScanWaveformCacheContract", "TestScanPauseResume", "TestImportPrecomputeWaveform", "TestCoverClickPerf", "TestStartupReconciliation", "TestProcessedFileExport", "TestSingleChannelExport", "TestSearchWorkspace", "TestMultiTabSync", "TestSubPlaylists", "TestSetPlaylistParent", "TestDatabaseFeatures")
        target_list = behavior.split("set(AURO_METADATA_LEGACY_TAG_CONTENT_TARGETS", 1)[1].split(")", 1)[0]
        self.assertEqual(sorted(targets), sorted(re.findall(r"\bTest[A-Za-z0-9_]+", target_list)))
        loop = behavior.split("foreach(_target IN LISTS AURO_METADATA_LEGACY_TAG_CONTENT_TARGETS", 1)[1].split("endforeach()", 1)[0]
        self.assertEqual(1, loop.count(selected))
        self.assertIn("target_sources(${_target} PRIVATE", loop)
        target = self._parenthesized_block(behavior, "add_executable(TestMetadataReaderContentEncoding")
        for token in ("tests/test_metadata_reader_content_encoding.cpp", "src/MetadataReader.cpp", "src/platform/filesystem_std.cpp"): self.assertEqual(1, target.count(token))
        self.assertNotRegex(target, r"text_encoding_(?:win32|std)\.cpp"); wiring = behavior.split("add_executable(TestMetadataReaderContentEncoding", 1)[1].split("add_test(NAME TestMetadataReaderContentEncoding", 1)[0]; self.assertEqual(["TestMetadataReaderContentEncoding"] * 2, re.findall(r"target_sources\((\w+)\s+PRIVATE", wiring)); self.assertEqual(1, wiring.count("src/platform/text_encoding_win32.cpp")); self.assertEqual(1, wiring.count("src/platform/text_encoding_std.cpp")); self.assertRegex(wiring, r"if\s*\(\s*WIN32\s*\)[\s\S]*target_sources\(TestMetadataReaderContentEncoding PRIVATE[\s\S]*text_encoding_win32\.cpp[\s\S]*else\s*\(\s*\)[\s\S]*target_sources\(TestMetadataReaderContentEncoding PRIVATE[\s\S]*text_encoding_std\.cpp")
        add_test = self._parenthesized_block(behavior, "add_test(NAME TestMetadataReaderContentEncoding")
        self.assertEqual(1, add_test.count("NAME TestMetadataReaderContentEncoding")); self.assertEqual(1, add_test.count("COMMAND TestMetadataReaderContentEncoding"))
        regression = (ROOT / "tools/behavior_regression_suite.py").read_text(encoding="utf-8")
        platform = regression.split('"platform-services",', 1)[1].split(")", 1)[0]
        self.assertEqual(1, platform.count("TestMetadataReaderContentEncoding"))

    def test_pro_tools_bridge_selects_filesystem_backend(self) -> None:
        cmake = re.sub(r"(?m)#.*$", "", (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))
        block = cmake.split("add_executable(HarkenSearchProToolsBridge", 1)[1].split("endif()", 1)[0]
        self.assertIn("src/platform/filesystem_std.cpp", block)
        self.assertIn("src/platform/filesystem_win32.cpp", block)
        self.assertEqual(1, block.count("src/platform/filesystem_std.cpp"))
        self.assertEqual(1, block.count("src/platform/filesystem_win32.cpp"))

    def test_cubase_bridge_stub_argv_uses_text_encoding_pal(self) -> None:
        source = (ROOT / "src" / "CubaseBridgeStubMain.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn('#include "platform/text_encoding.h"', source)
        self.assertNotIn("std::string WideToUtf8(", source)
        self.assertNotIn("WideCharToMultiByte", source)
        parse_args = source.split("std::map<std::string, std::string> ParseArgs", 1)[
            1
        ].split("std::string TrimLine", 1)[0]
        self.assertEqual(
            2, parse_args.count("Platform::TextEncoding::WideToUtf8Permissive(")
        )
        self.assertIn(
            "const std::wstring widePipe(pipeName.begin(), pipeName.end());", source
        )

        cmake = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(
            encoding="utf-8"
        )
        helper_wiring = cmake.split(
            "foreach(_auro_steinberg_helper IN ITEMS", 1
        )[1].split("endforeach()", 1)[0]
        self.assertEqual(1, helper_wiring.count("src/platform/text_encoding_win32.cpp"))

    def test_cubase_bridge_stub_uses_native_free_server_session(self) -> None:
        header = (ROOT / "src" / "platform" / "named_pipe.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src" / "CubaseBridgeStubMain.cpp").read_text(
            encoding="utf-8"
        )
        for token in ("class ServerSession", "ServerOperationResult Open(",
                      "ServerReadResult Read(", "WriteAndFlush(",
                      "DisconnectAndClose()", "std::unique_ptr<Impl> impl_"):
            self.assertIn(token, header)
        for token in ("windows.h", "HANDLE", "CreateNamedPipeW", "CloseHandle"):
            self.assertNotIn(token, header)
        self.assertIn('#include "platform/named_pipe.h"', source)
        self.assertIn('#include "platform/system.h"', source)
        self.assertIn("Platform::NamedPipe::ServerSession", source)
        self.assertIn("Platform::CurrentProcessId()", source)
        server = source.split("int wmain", 1)[1].split("return 0;", 1)[0]
        for token in ("while (!shouldExit)", "pipe.Open(widePipe, 4096, 4096, 1)",
                      "pipe.Accept()", "pipe.Read(4095)",
                      'helloPayload == "shutdown"', "shouldExit = true;",
                      '"ERR|SESSION_MISMATCH|handshake-token-mismatch\\n"'):
            self.assertIn(token, server)
        self.assertLess(server.index("pipe.WriteAndFlush(response);"),
                        server.rindex("pipe.DisconnectAndClose();"))
        for token in ("windows.h", "HANDLE", "CreateNamedPipeW", "ConnectNamedPipe",
                      "ReadFile", "WriteFile", "FlushFileBuffers",
                      "DisconnectNamedPipe", "CloseHandle", "GetLastError",
                      "GetCurrentProcessId"):
            self.assertNotIn(token, source)

    def test_cubase_adapter_current_process_path_uses_text_encoding_pal(self) -> None:
        source = (ROOT / "src" / "CubaseAdapter.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "platform/executable_path.h"', source)
        body = source.split("std::string CurrentProcessPathUtf8()", 1)[1].split(
            "BridgeExecutableResolution ResolveBundledBridgeHelper", 1
        )[0]
        self.assertIn("Platform::GetExecutablePath()", body)
        self.assertIn("ExecutablePathStatus::Success", body)
        self.assertIn("result.pathUtf8.empty()", body)
        self.assertIn("#else\n    return {};\n#endif", body)
        self.assertNotIn("GetModuleFileNameW", body)

        cmake = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(
            encoding="utf-8"
        )
        target = cmake.split(
            "add_executable(TestCubaseDirectInsertContract", 1
        )[1].split(")", 1)[0]
        self.assertEqual(1, target.count("tests/test_cubase_executable_path_stub.cpp"))
        self.assertNotIn("src/platform/executable_path_win32.cpp", target)

    def test_cubase_adapter_file_exists_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "CubaseAdapter.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "platform/filesystem.h"', source)
        body = source.split("bool FileExistsUtf8", 1)[1].split(
            "SteinbergHostKind DetectHostKindFromStrings", 1
        )[0]
        self.assertIn("if (path.empty()) return false;", body)
        self.assertIn("return Platform::FileSystem::Exists(path);", body)
        self.assertIn("catch (...)", body)
        for token in ("GetFileAttributesW", "fopen", "FILE*"):
            self.assertNotIn(token, body)

        cmake = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(
            encoding="utf-8"
        )
        wiring = cmake.split("foreach(_auro_diagnostics_test IN ITEMS", 1)[1].split(
            "endforeach()", 1
        )[0]
        self.assertIn("TestCubaseDirectInsertContract", wiring)
        self.assertEqual(1, wiring.count("src/platform/filesystem_std.cpp"))

    def test_cubase_adapter_launch_paths_fail_closed_before_process(self) -> None:
        source = (ROOT / "src" / "CubaseAdapter.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "platform/text_encoding.h"', source)
        self.assertNotIn("std::wstring Utf8PathToWide", source)
        launch = source.split("BridgeLaunchAttemptResult ExecuteBridgeLaunchAttempt", 1)[
            1
        ].split("BridgeEndpointWaitResult WaitForBridgeEndpoint", 1)[0]
        self.assertEqual(2, launch.count("TextEncoding::Utf8ToWidePermissive("))
        self.assertIn(
            "!plan.helperLaunchArguments.empty() && helperArgs.empty()", launch
        )
        guard = 'result.detail = "Bridge helper launch path conversion failed";'
        self.assertIn(guard, launch)
        self.assertLess(launch.index(guard), launch.index("g_bridgeLaunchHook"))
        self.assertLess(launch.index(guard), launch.index("Platform::LaunchProcess("))
        self.assertEqual(1, launch.count("if (!helperArgs.empty())"))
        self.assertNotIn("MultiByteToWideChar", launch)

    def test_cubase_process_launch_uses_pal_and_pal_test_seam(self) -> None:
        source = (ROOT / "src" / "CubaseAdapter.cpp").read_text(encoding="utf-8")
        launch = source.split("BridgeLaunchAttemptResult ExecuteBridgeLaunchAttempt", 1)[
            1
        ].split("BridgeEndpointWaitResult WaitForBridgeEndpoint", 1)[0]
        self.assertIn('#include "platform/process_launch.h"', source)
        self.assertNotIn("windows.h", source)
        self.assertEqual(1, launch.count("Platform::LaunchProcess("))
        self.assertIn("Platform::ProcessLaunchStatus::Ok", launch)
        self.assertIn("Platform::ProcessLaunchStatus::Failed", launch)
        self.assertIn("launch.processId", launch)
        self.assertIn("launch.systemError", launch)
        self.assertIn("err == 2 || err == 3", launch)
        self.assertIn('"Bridge helper process start failed with error "', launch)
        self.assertLess(launch.index("g_bridgeLaunchHook"),
                        launch.index("Platform::LaunchProcess("))
        for token in ("CreateProcessW", "GetLastError", "CloseHandle", "STARTUPINFOW",
                      "PROCESS_INFORMATION", "CREATE_NO_WINDOW"):
            self.assertNotIn(token, source)
        self.assertFalse((ROOT / "tests" / "cubase_adapter_process_launch_test_seam.h").exists())
        self.assertFalse((ROOT / "tests" / "cubase_adapter_process_launch_test_redirect.h").exists())
        cmake = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(
            encoding="utf-8"
        )
        target = cmake.split("add_executable(TestCubaseDirectInsertContract", 1)[1].split(
            "add_test(NAME TestCubaseDirectInsertContract", 1
        )[0]
        self.assertEqual(1, target.count("src/platform/process_launch_win32.cpp"))
        self.assertIn("AURO_PROCESS_LAUNCH_TESTING=1", target)

    def test_process_launch_pal_is_native_free_and_win32_owned(self) -> None:
        header = (ROOT / "src" / "platform" / "process_launch.h").read_text(
            encoding="utf-8"
        )
        win32 = (ROOT / "src" / "platform" / "process_launch_win32.cpp").read_text(
            encoding="utf-8"
        )
        for token in ("windows.h", "HANDLE", "DWORD", "STARTUPINFO", "PROCESS_INFORMATION"):
            self.assertNotIn(token, header)
        for token in ("ProcessLaunchStatus", "ProcessLaunchResult",
                      "ProcessLaunchOptions", "std::wstring_view applicationName",
                      "std::wstring_view commandLine", "LaunchProcess("):
            self.assertIn(token, header)
        for token in ("CreateProcessW", "GetLastError", "CloseHandle",
                      "CREATE_NO_WINDOW", "STARTUPINFOW", "PROCESS_INFORMATION"):
            self.assertIn(token, win32)
        body = win32.split("ProcessLaunchResult LaunchProcess(", 1)[1].split(
            "ProcessRunResult RunProcessAndWait(", 1
        )[0]
        self.assertIn("applicationName.find(L'\\0')", body)
        self.assertIn("commandLine.find(L'\\0')", body)
        self.assertLess(body.index("ProcessLaunchStatus::InvalidArgument"),
                        body.index("CreateProcessW"))
        self.assertEqual(2, body.count("CloseHandle("))
        run_body = win32.split("ProcessRunResult RunProcessAndWait(", 1)[1].split(
            "} // namespace Platform", 1
        )[0]
        for token in ("ProcessRunStatus::TimedOut", "WaitForSingleObject",
                      "TerminateProcess", "GetExitCodeProcess"):
            self.assertIn(token, run_body)
        enable = "#define AURO_PROCESS_LAUNCH_SEAM_ENABLE"
        disable = "#define AURO_PROCESS_LAUNCH_SEAM_DISABLE"
        redirect = '#include "tests/process_launch_test_redirect.h"'
        self.assertEqual(1, win32.count(enable))
        self.assertEqual(1, win32.count(disable))
        self.assertEqual(2, win32.count(redirect))
        self.assertFalse((ROOT / "src" / "platform" / "process_launch_test_seam.h").exists())
        self.assertTrue((ROOT / "tests" / "process_launch_test_seam.h").exists())
        self.assertLess(win32.index(enable), win32.index("ProcessLaunchResult LaunchProcess("))
        self.assertLess(win32.index("} // namespace Platform"), win32.index(disable))
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        tests_cmake = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(
            encoding="utf-8"
        )
        self.assertEqual(2, cmake.count("src/platform/process_launch_win32.cpp"))
        helper_cmake = cmake.split("add_executable(HarkenSearchProToolsBridge", 1)[1].split(
            "target_include_directories(HarkenSearchProToolsBridge", 1
        )[0]
        self.assertEqual(1, helper_cmake.count("src/platform/process_launch_win32.cpp"))
        target = tests_cmake.split("add_executable(TestPlatformProcessLaunch", 1)[1].split(
            "add_test(NAME TestPlatformProcessLaunch", 1
        )[0]
        self.assertEqual(1, target.count("src/platform/process_launch_win32.cpp"))
        self.assertIn("AURO_PROCESS_LAUNCH_TESTING=1", target)

    def test_process_launch_pal_has_macos_unsupported_parity(self) -> None:
        path = ROOT / "src" / "platform" / "process_launch_macos.cpp"
        self.assertTrue(path.exists())
        source = path.read_text(encoding="utf-8")
        self.assertIn('#include "process_launch.h"', source)
        self.assertIn("ProcessLaunchResult LaunchProcess(", source)
        self.assertIn("ProcessLaunchStatus::Unsupported", source)
        for token in ("CreateProcess", "NSTask", "posix_spawn", "fork(", "system("):
            self.assertNotIn(token, source)
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertEqual(1, cmake.count("src/platform/process_launch_macos.cpp"))

    def test_cubase_endpoint_wait_uses_system_sleep_pal_in_order(self) -> None:
        source = (ROOT / "src" / "CubaseAdapter.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "platform/system.h"', source)
        body = source.split("BridgeEndpointWaitResult WaitForBridgeEndpoint", 1)[
            1
        ].split("BridgeAttachPreparationResult PrepareBridgeAttach", 1)[0]
        probe = body.index("const auto probe = ProbeBridgeEndpoint(endpoint);")
        final_break = body.index("if (waitedMs == plan.endpointWaitTimeoutMs)")
        sleep = body.index(
            "Platform::SleepMilliseconds(static_cast<uint32_t>(pollIntervalMs));"
        )
        increment = body.index("waitedMs = std::min(")
        self.assertLess(probe, final_break)
        self.assertLess(final_break, sleep)
        self.assertLess(sleep, increment)
        self.assertIn("#ifdef _WIN32", body)
        self.assertNotIn("Sleep(", body)

    def test_cubase_pipe_client_transport_is_fault_characterized(self) -> None:
        source = (ROOT / "src" / "CubaseAdapter.cpp").read_text(encoding="utf-8")
        platform_source = (ROOT / "src" / "platform" / "named_pipe_win32.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn('#include "platform/named_pipe.h"', source)
        body = source.split("bool SendPipeMessage", 1)[1].split("} // namespace", 1)[0]
        for assignment in (
            "options.connectAttempts = 5", "options.busyWaitMs = 250",
            "options.notFoundSleepMs = 25", "options.responsePollMs = 10",
            "options.responseCapacity = 2047", "options.ignoreMessageModeFailure = true",
            "options.continueAfterPeekOrReadFailure = true", "options.trimTrailingCrLf = true",
        ):
            self.assertIn(assignment, body)
        self.assertIn("Platform::NamedPipe::ExchangeMessageWithOptions", body)
        self.assertIn("exchange.retryExhausted", body)
        for detail in (
            "ERR|Cannot connect to Steinberg bridge (error ",
            "ERR|Steinberg bridge pipe is not available",
            "ERR|Failed to write to Steinberg bridge pipe",
            "ERR|Steinberg bridge did not respond",
            "ERR|Steinberg bridge transport is only implemented on Windows",
        ):
            self.assertIn(detail, body)
        for token in ("CreateFileW", "GetLastError", "WaitNamedPipeW", "Sleep(",
                      "SetNamedPipeHandleState", "WriteFile", "PeekNamedPipe", "ReadFile",
                      "CloseHandle", "GetTickCount"):
            self.assertNotIn(token, body)
        for token in ("CreateFileW", "WaitNamedPipeW", "SetNamedPipeHandleState",
                      "WriteFile", "PeekNamedPipe", "ReadFile", "CloseHandle"):
            self.assertIn(token, platform_source)
        self.assertFalse((ROOT / "tests" / "cubase_adapter_pipe_client_test_seam.h").exists())
        self.assertFalse((ROOT / "tests" / "cubase_adapter_pipe_client_test_redirect.h").exists())

    def test_windows_main_runtime_paths_use_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        runtime_body = source.split("std::string GetHarkenSearchRuntimeRootUtf8", 1)[1].split(
            "static void", 1
        )[0]
        cold_body = source.split("static std::string CanonicalColdMiniPath", 1)[1].split(
            "static bool", 1
        )[0]
        self.assertIn('#include "platform/filesystem.h"', source)
        self.assertIn("Platform::FileSystem::CreateDirectories", runtime_body)
        self.assertIn("Platform::FileSystem::WeaklyCanonical", cold_body)
        self.assertNotIn("std::filesystem::create_directories", runtime_body)
        self.assertNotIn("std::filesystem::weakly_canonical", cold_body)

    def test_windows_main_utf8_diagnostics_use_text_encoding_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "platform/text_encoding.h"', source)
        self.assertGreaterEqual(source.count("Platform::TextEncoding::Utf8ToWidePermissive"), 3)
        self.assertNotIn("MultiByteToWideChar", source)

    def test_main_browser_folder_queries_use_directory_iterator_pal(self) -> None:
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        runtime = (ROOT / "src/BrowserSystemFolderSearchRuntime.cpp").read_text(
            encoding="utf-8"
        )
        body = main.split("static BrowserSystemFolderCacheEntry& GetBrowserSystemFolderChildren", 1)[1].split(
            "static std::vector<aura::browser::SystemFolderSearchRoot> BrowserSystemFolderSearchRoots", 1
        )[0]
        self.assertIn("Platform::FileSystem::DirectoryIterator", body + runtime)
        self.assertNotIn("fs::directory_iterator", body + runtime)
        self.assertNotIn("fs::recursive_directory_iterator", body + runtime)

    def test_windows_main_startup_storage_directories_use_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split(
            "// Development mode keeps mutable state beside the executable.", 1
        )[1].split("io.IniFilename", 1)[0]

        self.assertIn(
            'if (!Platform::FileSystem::EnsureDirectoryExists(runtimeRootUtf8 + "\\\\database"))',
            body,
        )
        self.assertIn(
            "if (!Platform::FileSystem::EnsureDirectoryExists(appDataRoot))", body
        )
        self.assertEqual(2, body.count("Platform::FileSystem::EnsureDirectoryExists("))
        self.assertEqual(2, body.count("throw std::runtime_error("))
        self.assertNotIn("std::filesystem::create_directories", body)

    def test_windows_main_startup_avatar_reads_use_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        for helper in ("ReadStartupAvatarSvgFile", "ReadBinaryFile"):
            body = source.split(f"static bool {helper}", 1)[1].split("\n}", 1)[0]
            self.assertIn("Platform::FileSystem::ReadFileBytes(pathUtf8)", body)
            self.assertNotIn("std::ifstream", body)
            self.assertNotIn("Utf8ToWide(pathUtf8)", body)

    def test_windows_main_drops_unused_binary_writer(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        self.assertNotIn("static bool WriteFileBinaryUtf8", source)

    def test_windows_main_bug_report_uses_path_and_stream_pals(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("void WriteBugReportJson", 1)[1].split(
            "std::string BuildSysInfo", 1
        )[0]
        self.assertIn('#include "platform/executable_path.h"', source)
        self.assertIn("Platform::GetExecutablePath()", body)
        self.assertIn("Platform::ExecutablePathStatus::Success", body)
        self.assertIn("Platform::FileSystem::EnsureDirectoryExists", body)
        self.assertIn("std::ostringstream payload;", body)
        self.assertIn("Platform::FileSystem::StreamWriter writer;", body)
        self.assertIn("writer.Open(PathToUtf8(filePath), false, true)", body)
        self.assertIn("const std::string payloadText = payload.str()", body)
        self.assertIn("writer.Write(payloadText)", body)
        self.assertIn("writer.Close()", body)
        self.assertLess(body.index("payload << \"{\\n\""), body.index("writer.Open("))
        self.assertLess(body.index("writer.Open("), body.index("writer.Write("))
        self.assertLess(body.index("writer.Write("), body.index("writer.Close()"))
        self.assertNotIn("GetModuleFileNameW", body)
        self.assertNotIn("fs::create_directories", body)
        self.assertNotIn("std::ofstream", body)

    def test_cover_discovery_boundary_is_defined_before_consumer_migration(self) -> None:
        header = ROOT / "src" / "platform" / "cover_discovery.h"
        std_source = ROOT / "src" / "platform" / "cover_discovery_std.cpp"
        consumer = (ROOT / "src" / "CoverModule.cpp").read_text(encoding="utf-8")
        self.assertTrue(header.is_file(), "cover discovery boundary header is missing")
        self.assertTrue(std_source.is_file(), "cover discovery std backend is missing")
        header_text = header.read_text(encoding="utf-8")
        self.assertIn("namespace Platform::CoverDiscovery", header_text)
        self.assertIn("enum class Status", header_text)
        self.assertIn("Result Find(", header_text)
        self.assertIn('#include "platform/cover_discovery.h"', consumer)
        discovery_body = consumer.split("static bool FindFolderCoverImagePathFast", 1)[1].split(
            "static void", 1
        )[0]
        for token in (
            "std::filesystem::directory_iterator",
            "std::filesystem::recursive_directory_iterator",
            "fs::file_size",
            "fs::exists",
        ):
            self.assertNotIn(token, discovery_body)

    def test_cover_discovery_probe_errors_are_not_negative_cached(self) -> None:
        source = (ROOT / "src" / "CoverModule.cpp").read_text(encoding="utf-8")
        body = source.split("static bool FindFolderCoverImagePathFast", 1)[1].split(
            "static void", 1
        )[0]
        self.assertIn(
            "if (result.status == Platform::CoverDiscovery::Status::ProbeError) return false;",
            body,
        )

    def test_cover_diagnostics_links_cover_discovery_backend(self) -> None:
        cmake = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
        target = cmake.split("add_executable(TestCoverDiagnostics", 1)[1].split(
            "add_test(NAME TestCoverDiagnostics", 1
        )[0]
        self.assertEqual(1, target.count("src/platform/cover_discovery_std.cpp"))
        self.assertEqual(1, target.count("src/platform/text_encoding_win32.cpp"))
        self.assertEqual(1, target.count("src/platform/text_encoding_std.cpp"))

    def test_cover_discovery_sibling_strict_priority_is_explicit(self) -> None:
        backend = (ROOT / "src" / "platform" / "cover_discovery_std.cpp").read_text(
            encoding="utf-8"
        )
        body = backend.split("Result FindStrictOnly", 1)[1].split("Result FindLocal", 1)[0]
        self.assertIn("for (std::string_view name : kStrictNames)", body)
        self.assertIn("for (std::string_view extension : kStrictExtensions)", body)

    def test_qa_atomic_file_boundary_is_defined_before_consumer_migration(self) -> None:
        header = ROOT / "src" / "platform" / "qa_files.h"
        backend = ROOT / "src" / "platform" / "qa_files_std.cpp"
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        self.assertTrue(header.is_file(), "QA file boundary header is missing")
        self.assertTrue(backend.is_file(), "QA file std backend is missing")
        header_text = header.read_text(encoding="utf-8")
        self.assertIn("namespace Platform::QaFiles", header_text)
        self.assertIn("bool ReadCommandJson(", header_text)
        self.assertIn("bool WriteResultAtomically(", header_text)
        self.assertIn('#include "platform/qa_files.h"', source)
        read_body = source.split("static void PollQaCommandFile", 1)[1].split(
            "// Minimal JSON parsing", 1
        )[0]
        self.assertIn("Platform::QaFiles::ReadCommandJson", read_body)
        write_body = source.split("const std::string tmpSuffix", 1)[1].split(
            "DIAG_ACTION_FMT", 1
        )[0]
        self.assertIn("Platform::QaFiles::WriteResultAtomically", write_body)
        for token in ("FILE*", "fopen_s", "fopen(", "fread", "fwrite", "fclose", "std::remove", "std::rename"):
            self.assertNotIn(token, read_body + write_body)

    def test_cover_module_residual_platform_findings_are_retired(self) -> None:
        source = (ROOT / "src" / "CoverModule.cpp").read_text(encoding="utf-8")
        for include in ("windows.h", "wincodec.h", "shlwapi.h"):
            self.assertNotIn(f"#include <{include}>", source)
        self.assertNotIn('#pragma comment(lib, "shlwapi.lib")', source)
        art_fallback = source.split("if (!hasArt)", 1)[1].split("if (!hasArt)", 1)[0]
        self.assertIn("Platform::FileSystem::Exists", art_fallback)
        self.assertNotIn("fs::exists", art_fallback)
        normalize = source.split("static std::string NormalizePathForCoverKey", 1)[1].split(
            "std::string CanonicalCoverKey", 1
        )[0]
        self.assertIn("Platform::FileSystem::Exists(pathUtf8)", normalize)
        self.assertIn("Platform::FileSystem::WeaklyCanonical(pathUtf8)", normalize)
        self.assertNotIn("fs::canonical", normalize)

    def test_windows_main_drop_file_size_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("// Process files - add individual files to database", 1)[1].split(
            "return 0;", 1
        )[0]
        self.assertIn("Platform::FileSystem::ProbeRegularFile(filePath)", body)
        self.assertIn("record.fileSize", body)
        self.assertNotIn("std::filesystem::file_size", body)

    def test_windows_main_metadata_patch_exists_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split('cmd == "metadata_apply_patch"', 1)[1].split(
            'cmd == "metadata_read"', 1
        )[0]
        self.assertIn("Platform::FileSystem::Exists(rec.filePath)", body)
        self.assertNotIn("fs::exists(Utf8ToPath(rec.filePath)", body)

    def test_main_metadata_and_relink_module_exists_checks_use_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        relink_source = (ROOT / "src" / "RelinkWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn('#include "platform/filesystem.h"', source)
        metadata = source.split("static bool SaveInfoMetadataDraft(", 1)[1].split(
            "void RenderInfoMetadataLeavePrompt(", 1
        )[0]
        self.assertIn("Platform::FileSystem::Exists(original.filePath)", metadata)
        self.assertNotIn("fs::exists(Utf8ToPathMain(rec->filePath)", metadata)
        self.assertNotIn("fs::exists(Utf8ToPathMain(record.filePath)", metadata)
        self.assertIn("aura::ui::EvaluateRelinkForRecord(state, record)", source)
        self.assertIn('#include "platform/filesystem.h"', relink_source)
        self.assertIn(
            "Platform::FileSystem::Exists(record.filePath)", relink_source
        )
        self.assertNotIn("fs::exists", relink_source)
        self.assertIn('state.metadataBatchMode?"Batch edit metadata":"Edit metadata"', metadata)
        self.assertIn("for(const auto& original:state.infoEditRecords)", metadata)
        rescan_adapter = source.split(
            "class AppSelectedRecordRescanOperationPort", 1
        )[1].split("#ifdef _WIN32", 1)[0]
        self.assertIn("Platform::FileSystem::ProbeRegularFile(filePath)", rescan_adapter)
        self.assertNotIn("fs::exists", rescan_adapter)

    def test_database_backup_restore_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        backup = source.split("bool Database::backupDatabase", 1)[1].split(
            "bool Database::restoreDatabase", 1
        )[0]
        restore = source.split("bool Database::restoreDatabase", 1)[1].split(
            "// Startup health check", 1
        )[0]

        self.assertEqual(3, backup.count("Platform::FileSystem::CopyFile("))
        self.assertEqual(2, backup.count("Platform::FileSystem::Exists("))
        self.assertEqual(2, restore.count("Platform::FileSystem::CopyFile("))
        for body in (backup, restore):
            self.assertNotIn("fs::copy_file", body)
            self.assertNotIn("fs::exists", body)

        retired = {
            "src/Database.cpp|filesystem-io|fs::copy_file|c98c8cd4a9b16c6b|1",
            "src/Database.cpp|filesystem-io|fs::exists|5cc6b01610d343ff|1",
            "src/Database.cpp|filesystem-io|fs::copy_file|53b92daa7a1cbb0b|1",
            "src/Database.cpp|filesystem-io|fs::exists|e903cbc03157468c|1",
            "src/Database.cpp|filesystem-io|fs::copy_file|470d8134a7126f58|1",
            "src/Database.cpp|filesystem-io|fs::copy_file|a31bea3e3bd71e76|1",
            "src/Database.cpp|filesystem-io|fs::copy_file|ab433c1652920a45|1",
        }
        current = {finding.fingerprint for finding in boundary.scan_tree(ROOT)}
        self.assertTrue(retired.isdisjoint(current), retired & current)

    def test_database_relink_missing_checks_use_probe_path_pal(self) -> None:
        source = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        orphan = source.split("Database::findOrphanByFingerprint", 1)[1].split(
            "Database::findRelinkCandidates", 1
        )[0]
        candidates = source.split("Database::findRelinkCandidates", 1)[1].split(
            "if (orphans.empty())", 1
        )[0]

        for body in (orphan, candidates):
            self.assertIn("Platform::FileSystem::ProbePath(dbPath)", body)
            self.assertIn("Platform::FileSystem::PathStatus::Missing", body)
            self.assertNotIn("std::filesystem::exists(Utf8ToPath(dbPath))", body)

    def test_database_relink_search_uses_directory_iterator_pal(self) -> None:
        source = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        body = source.split("Database::findRelinkCandidates", 1)[1].split(
            "void Database::mergeLibraryRoots", 1
        )[0]
        self.assertIn("Platform::FileSystem::DirectoryIterator", body)
        self.assertNotIn("std::filesystem::recursive_directory_iterator", body)

    def test_main_path_mtime_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static int64_t PathMtimeUnixSecondsMain", 1)[1].split(
            "// GetDatabaseDisplayNameUtf8", 1
        )[0]
        self.assertIn("Platform::FileSystem::LastWriteTimeUnixSeconds(PathToUtf8Main(p))", body)
        self.assertNotIn("fs::exists", body)
        self.assertNotIn("fs::last_write_time", body)

    def test_main_rescan_file_stats_use_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("class AppSelectedRecordRescanOperationPort", 1)[1].split(
            "#ifdef _WIN32", 1
        )[0]

        self.assertIn("Platform::FileSystem::ProbeRegularFile(filePath)", body)
        self.assertIn("record.fileSize = static_cast<int64_t>(update.fileSize);", body)
        self.assertIn("Platform::FileSystem::LastWriteTimeUnixSeconds(filePath)", body)
        self.assertNotIn("fs::file_size", body)
        self.assertNotIn("fs::last_write_time", body)

    def test_cover_rebuild_mtime_uses_pal_fallback_without_preflight(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split(
            "class AppCoverRebuildFolderLookupPort", 1
        )[1].split("static void PumpCoverRebuild", 1)[0]
        self.assertIn("PathMtimeUnixSecondsMain(Utf8ToPathMain(folderSrcPath))", body)
        self.assertNotIn("fs::exists", body)

    def test_cover_clipboard_sidecar_probe_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static bool CopyCoverToClipboard", 1)[1].split(
            "static void ShowCoverClipboardMessage", 1
        )[0]
        self.assertIn("Platform::FileSystem::ProbeRegularFile", body)
        self.assertIn("imageInfo.isRegularFile", body)
        self.assertNotIn("fs::exists", body)
        self.assertNotIn("fs::is_regular_file", body)

    def test_main_waveform_diagnostic_totals_use_standard_uint64(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("const int wfCh = wfPtr ?", 1)[1].split(
            "#endif", 1
        )[0]
        self.assertIn("const uint64_t wfImpliedTotal", body)
        self.assertIn("uint64_t cursorFrames", body)
        self.assertIn("static_cast<uint64_t>(wfSpp)", body)
        self.assertIn("static_cast<uint64_t>(wfFrames)", body)
        self.assertIn("static_cast<uint64_t>(playPos * (float)fileInfo.totalFrames)", body)
        self.assertNotIn("ma_uint64 wfImpliedTotal", body)
        self.assertNotIn("ma_uint64 cursorFrames", body)
        self.assertNotIn("(ma_uint64)wfSpp", body)
        self.assertNotIn("(ma_uint64)wfFrames", body)
        self.assertNotIn("(ma_uint64)(playPos", body)

    def test_main_leading_silence_loop_indices_use_standard_types(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static bool DetectLeadingSilenceFromSourceFile", 1)[1].split(
            "void MaybeSkipLeadingSilence", 1
        )[0]
        self.assertIn("const uint64_t maxScanFrames = static_cast<uint64_t>(std::max", body)
        self.assertIn("constexpr uint64_t kChunkFrames = 8192;", body)
        self.assertIn("uint64_t framesScanned = 0;", body)
        self.assertIn(
            "const uint64_t framesToRead = std::min<uint64_t>(kChunkFrames, maxScanFrames - framesScanned);",
            body,
        )
        self.assertIn("for (uint64_t frame = 0;", body)
        self.assertIn("for (uint32_t ch = 0;", body)
        self.assertIn(
            "const auto readResult = Platform::AudioDataSource::ReadPcmFrames(",
            body,
        )
        self.assertIn("const auto framesRead = readResult.framesRead;", body)
        self.assertIn("readResult.backendCode != MA_SUCCESS", body)
        self.assertIn(
            "Platform::AudioDataSource::BorrowDataSource(decoder),",
            body,
        )
        self.assertNotIn("ma_decoder_read_pcm_frames(", body)
        self.assertNotIn("const ma_uint64 maxScanFrames", body)
        self.assertNotIn("(ma_uint64)std::max", body)
        self.assertNotIn("constexpr ma_uint64 kChunkFrames", body)
        self.assertNotIn("ma_uint64 framesScanned", body)
        self.assertNotIn("const ma_uint64 framesToRead", body)
        self.assertNotIn("std::min<ma_uint64>(kChunkFrames", body)
        self.assertNotIn("for (ma_uint64 frame = 0;", body)
        self.assertNotIn("for (ma_uint32 ch = 0;", body)

    def test_main_decoder_scalar_types_use_standard_uint32(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        silence_body = source.split("static bool DetectLeadingSilenceFromSourceFile", 1)[1].split(
            "void MaybeSkipLeadingSilence", 1
        )[0]
        spectrogram_body = source.split("static bool GenerateSpectrogramPixels", 1)[1].split(
            "if (outDecodedFrames)", 1
        )[0]

        self.assertIn("const uint32_t channels = format.channels;", silence_body)
        self.assertIn("const uint32_t sampleRate = format.sampleRate;", silence_body)
        self.assertNotIn("ma_uint32 channels", silence_body)
        self.assertNotIn("ma_uint32 sampleRate", silence_body)
        self.assertIn("uint32_t engineSampleRate = 0", spectrogram_body)
        self.assertIn("const uint32_t decodeSR =", spectrogram_body)
        self.assertNotIn("ma_uint32 engineSampleRate", spectrogram_body)
        self.assertNotIn("const ma_uint32 decodeSR", spectrogram_body)

    def test_main_spectrogram_downmix_indices_use_standard_types(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static bool GenerateSpectrogramPixels", 1)[1].split(
            "if (outDecodedFrames)", 1
        )[0]
        self.assertIn("for (uint64_t f = 0;", body)
        self.assertIn("for (uint32_t c = 0;", body)
        self.assertNotIn("for (ma_uint64 f = 0;", body)
        self.assertNotIn("for (ma_uint32 c = 0;", body)

    def test_main_spectrogram_channel_count_uses_standard_type(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static bool GenerateSpectrogramPixels", 1)[1].split(
            "if (outDecodedFrames)", 1
        )[0]
        self.assertIn(
            "const uint32_t nCh = decoderFormat.channels > 0 ? decoderFormat.channels : 1;",
            body,
        )
        self.assertNotIn(
            "const ma_uint32 nCh = decoderFormat.channels > 0 ? decoderFormat.channels : 1;",
            body,
        )

    def test_main_spectrogram_decoder_reads_use_data_source_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_data_source_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("static bool GenerateSpectrogramPixels", 1)[1].split(
            "if (outDecodedFrames)", 1
        )[0]

        self.assertIn("LengthInFrames QueryLength(BorrowedSource source) noexcept;", header)
        self.assertIn("ReadResult ReadPcmFrames(", header)
        self.assertIn(
            "const auto totalFrames =",
            body,
        )
        self.assertIn("Platform::AudioDataSource::QueryLength(", body)
        self.assertIn("Platform::AudioDataSource::BorrowDataSource(decoder)).frames", body)
        self.assertIn(
            "const auto readResult = Platform::AudioDataSource::ReadPcmFrames(",
            body,
        )
        self.assertIn("const auto got = readResult.framesRead;", body)
        self.assertNotIn("ma_decoder_get_length_in_pcm_frames(", body)
        self.assertNotIn("ma_decoder_read_pcm_frames(", body)
        self.assertIn("ma_data_source_get_length_in_pcm_frames", backend)
        self.assertIn("ma_data_source_read_pcm_frames", backend)

    def test_main_decoder_cleanup_uses_data_source_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_data_source_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "platform/audio_data_source.h"', source)
        self.assertIn("void UninitializeDecoder(BorrowedDecoder decoder) noexcept;", header)
        self.assertIn("OperationResult InitializeDecoderFromFileWithFormat(", header)
        self.assertIn("OperationResult InitializeDecoderFromFileWithFormat(", backend)
        self.assertIn("Platform::AudioDataSource::InitializeDecoderFromFileWithFormat(", source)
        self.assertEqual(5, source.count("Platform::AudioDataSource::UninitializeDecoder("))
        self.assertEqual(1, source.count("Platform::AudioDataSource::BorrowDecoder(decoder)"))
        self.assertNotIn("Platform::AudioDataSource::BorrowDecoder(&decoder)", source)
        self.assertNotIn("ma_decoder_uninit(", source)
        self.assertIn("ma_decoder_uninit", backend)

    def test_audio_engine_native_rate_decoder_initialization_uses_audio_data_source_pal(self) -> None:
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        functions = (
            ("bool AudioEngine::DecodeFileToInterleavedF32(", "bool AudioEngine::generateWaveformForFile("),
            ("bool AudioEngine::generateWaveformForFile(", "bool AudioEngine::generateWaveformFromOpenDecoder("),
            ("bool AudioEngine::generateQuickWaveformForFile(", "bool AudioEngine::decodeFullFileToMemory("),
        )
        for start, end in functions:
            body = source.split(start, 1)[1].split(end, 1)[0]
            self.assertIn("Platform::AudioDataSource::InitializeDecoderFromFile(", body)
            self.assertNotIn("ma_decoder_config_init(", body)
            self.assertNotIn("ma_decoder_init_file_w(", body)
            self.assertNotIn("ma_decoder_init_file(", body)

    def test_audio_engine_probe_decoder_initialization_uses_audio_data_source_pal(self) -> None:
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        functions = (
            ("bool AudioEngine::buildWaveform(", "bool AudioEngine::buildQuickWaveform("),
            ("bool AudioEngine::buildQuickWaveform(", "void AudioEngine::waveformWorkerLoop()"),
            ("bool AudioEngine::loadFile(", "void AudioEngine::loadFileWorkerLoop()"),
            ("bool AudioEngine::exportFileProcessed(", "bool AudioEngine::DecodeFileToInterleavedF32("),
            ("bool AudioEngine::exportSingleChannel(", "bool AudioEngine::exportFileProcessed("),
        )
        for start, end in functions:
            body = source.split(start, 1)[1].split(end, 1)[0]
            self.assertIn("Platform::AudioDataSource::InitializeDecoderFromFile(", body)

    def test_audio_engine_channel_map_decoders_use_owned_pal_storage(self) -> None:
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        functions = (
            ("bool AudioEngine::buildWaveform(", "bool AudioEngine::buildQuickWaveform("),
            ("bool AudioEngine::buildQuickWaveform(", "void AudioEngine::waveformWorkerLoop()"),
            ("void AudioEngine::waveformWorkerLoop()", "bool AudioEngine::loadFile("),
            ("bool AudioEngine::exportRegion(", "bool AudioEngine::hasAudioAdjustments("),
            ("bool AudioEngine::exportFileProcessed(", "bool AudioEngine::DecodeFileToInterleavedF32("),
        )
        for start, end in functions:
            body = source.split(start, 1)[1].split(end, 1)[0]
            self.assertIn("Platform::AudioDataSource::Decoder decoder;", body)
            self.assertNotIn("ma_decoder decoder;", body)
            self.assertIn(
                "Platform::AudioDataSource::InitializeDecoderFromFileWithFormatAndChannelMap(",
                body,
            )

    def test_audio_engine_fixed_format_decoder_initialization_uses_audio_data_source_pal(self) -> None:
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        decode_body = source.split("bool AudioEngine::decodeFullFileToMemory(", 1)[1].split(
            "bool AudioEngine::switchOutputDevice(", 1
        )[0]
        export_body = source.split("bool AudioEngine::exportSingleChannel(", 1)[1].split(
            "bool AudioEngine::exportFileProcessed(", 1
        )[0]

        self.assertIn(
            "Platform::AudioDataSource::InitializeDecoderFromFileWithFormat(",
            decode_body,
        )
        self.assertIn(
            "Platform::AudioDataSource::InitializeDecoderFromFileWithFormat(",
            export_body,
        )
        for body in (decode_body, export_body):
            self.assertNotIn("ma_decoder_config_init(", body)
            self.assertNotIn("ma_decoder_init_file_w(", body)
            self.assertNotIn("ma_decoder_init_file(", body)

    def test_audio_engine_reverse_reload_decoder_initialization_uses_audio_data_source_pal(self) -> None:
        source = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
        body = source.split("bool AudioEngine::reloadWithReverseState()", 1)[1].split(
            "void AudioEngine::setInPoint", 1
        )[0]
        self.assertIn(
            "Platform::AudioDataSource::InitializeDecoderFromFileWithFormat(", body
        )
        self.assertNotIn("ma_decoder_config_init(", body)
        self.assertNotIn("ma_decoder_init_file_w(", body)
        self.assertNotIn("ma_decoder_init_file(", body)

    def test_main_spectrogram_engine_rate_snapshot_uses_standard_type(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("const std::string reqPath = s_requestedPath", 1)[1].split(
            "s_future = std::async", 1
        )[0]
        self.assertIn(
            "const uint32_t reqEngineSR = state.audioEngine.getEngineSampleRate();",
            body,
        )
        self.assertNotIn(
            "const ma_uint32 reqEngineSR = state.audioEngine.getEngineSampleRate();",
            body,
        )

    def test_main_waveform_alignment_diagnostic_uses_standard_width_arithmetic(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split('"ALIGN: totalF=', 1)[1].split("#endif", 1)[0]
        self.assertIn(
            "(uint64_t)dSpp * (uint64_t)dPeaks",
            body,
        )
        self.assertNotIn("(ma_uint64)dSpp * (ma_uint64)dPeaks", body)

    def test_windows_main_spectrogram_cache_uses_standard_frame_count(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("struct SpectrogramDecodeCache", 1)[1].split("};", 1)[0]
        self.assertIn("uint64_t framesRead = 0;", body)
        self.assertNotIn("ma_uint64 framesRead = 0;", body)

    def test_windows_main_spectrogram_read_limit_uses_standard_frame_count(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("static bool DecodeMonoSamples44k1Cached", 1)[1].split(
            "static bool GenerateSpectrogramTileRGBA", 1
        )[0]
        self.assertIn(
            "const auto lengthResult = Platform::AudioDataSource::QueryLength(",
            body,
        )
        self.assertIn("const auto totalFrames = lengthResult.frames;", body)
        self.assertIn(
            "const auto readResult = Platform::AudioDataSource::ReadPcmFrames(",
            body,
        )
        self.assertIn("const auto framesRead = readResult.framesRead;", body)
        self.assertIn(
            "const uint64_t framesToRead = std::min<uint64_t>(static_cast<uint64_t>(maxFrames), "
            "static_cast<uint64_t>(totalFrames));",
            body,
        )
        self.assertNotIn(
            "int framesToRead = (int)std::min((ma_uint64)maxFrames, totalFrames);",
            body,
        )
        self.assertNotIn("ma_decoder_get_length_in_pcm_frames(", body)
        self.assertNotIn("ma_decoder_read_pcm_frames(", body)

    def test_windows_main_decoder_cleanup_uses_data_source_boundary(self) -> None:
        header = (ROOT / "src" / "platform" / "audio_data_source.h").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "platform" / "audio_data_source_miniaudio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "platform/audio_data_source.h"', source)
        self.assertIn("void UninitializeDecoder(BorrowedDecoder decoder) noexcept;", header)
        self.assertIn("OperationResult InitializeDecoderFromFileWithFormat(", header)
        self.assertIn("OperationResult InitializeDecoderFromFileWithFormat(", backend)
        self.assertIn("Platform::AudioDataSource::InitializeDecoderFromFileWithFormat(", source)
        self.assertEqual(2, source.count("Platform::AudioDataSource::UninitializeDecoder("))
        self.assertEqual(2, source.count("Platform::AudioDataSource::BorrowDataSource(decoder)"))
        self.assertNotIn("Platform::AudioDataSource::BorrowDecoder(&decoder)", source)
        self.assertNotIn("ma_decoder_uninit(", source)
        self.assertIn("ma_decoder_uninit", backend)

    def test_windows_main_spectrogram_tile_uses_standard_frame_count(self) -> None:
        source = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("static bool GenerateSpectrogramTileRGBA", 1)[1].split(
            "static std::unordered_map<std::string, std::string> ParseQueryLikePayload", 1
        )[0]
        signature = body.split(") {", 1)[0]
        self.assertIn("uint64_t framesRead", signature)
        self.assertNotIn("ma_uint64 framesRead", signature)
        self.assertIn("framesRead < (uint64_t)fftSize", body)
        self.assertNotIn("framesRead < (ma_uint64)fftSize", body)

    def test_main_background_workers_use_thread_priority_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        registry_body = source.split(
            "static std::vector<HarkenSearch::DawBridge::InstallStatus> ProbeDawBridgeRegistrySnapshots",
            1,
        )[1].split("static void PumpDawBridgeRegistryProbeCache", 1)[0]
        self.assertIn("Platform::SetThreadBackgroundPriority()", registry_body)
        self.assertNotIn("SetThreadPriority(", registry_body)

    def test_file_player_deferred_db_warmup_runtime_owns_async_session(self) -> None:
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        runtime = (ROOT / "src/FilePlayerDeferredDbWarmupRuntime.cpp").read_text(
            encoding="utf-8"
        )
        operation = (ROOT / "src/FilePlayerDeferredDbWarmupOperation.cpp").read_text(
            encoding="utf-8"
        )
        warmup_body = main.split(
            "static void StartFilePlayerDeferredDbWarmup", 1
        )[1].split("static bool MountFilePlayerDatabaseForInteractiveRefresh", 1)[0]

        self.assertEqual(1, operation.count("Platform::SetThreadBackgroundPriority()"))
        self.assertNotIn("SetThreadPriority(", operation)
        self.assertNotIn("SetThreadPriority(", runtime)
        self.assertNotIn("platform/system_win32", runtime + operation)
        self.assertIn("!state.filePlayerFastPathActive || state.database.isOpen()", warmup_body)
        self.assertIn("ResolveFilePlayerDatabasePath(state)", warmup_body)
        self.assertIn("g_filePlayerDeferredDbWarmupRuntime.Start(dbPathUtf8)", warmup_body)
        self.assertIn("g_filePlayerDeferredDbWarmupRuntime.PollFinished(success)", warmup_body)
        for removed in (
            "s_filePlayerDeferredDbWarmup",
            "std::thread(",
            "Database warmupDb",
            "openReadOnly",
        ):
            self.assertNotIn(removed, warmup_body)

    def test_file_player_database_open_workflow_owns_fallback_policy(self) -> None:
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        core = (ROOT / "src/FilePlayerDatabaseOpenWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        adapter = (ROOT / "src/FilePlayerDatabaseOpenAdapter.cpp").read_text(
            encoding="utf-8"
        )
        mount_body = main.split(
            "static bool MountFilePlayerDatabaseForInteractiveRefresh", 1
        )[1].split("void RefreshSearchResults", 1)[0]

        self.assertIn("fallbackPathUtf8 == result.activePathUtf8", core)
        self.assertIn("result.fallbackAttempted = true", core)
        self.assertIn("database.open(pathUtf8, true)", adapter)
        self.assertIn("database.getLastError()", adapter)
        self.assertNotIn("state.database.open(", mount_body)
        self.assertNotIn("state.database.getLastError()", mount_body)
        self.assertIn("OpenFilePlayerDatabaseWithFallback(", mount_body)
        for retained in (
            "state.loadErrorToast",
            "state.filterQueryState = FilterQueryState::Error",
            "ConfigureDatabaseRuntimeForFilePlayerMount(state, dbPathUtf8)",
            "state.database.preheat()",
            "state.searchWorker.start(&state.database)",
            "state.waveformWorker.start(&state.database",
            "StartDeferredSidebarBootstrap(state, true, true)",
        ):
            self.assertIn(retained, mount_body)

    def test_database_switch_worker_runtime_owns_thread_publication(self) -> None:
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        runtime = (ROOT / "src/DatabaseSwitchWorkerRuntime.cpp").read_text(
            encoding="utf-8"
        )
        switch_body = main.split(
            "void WaitForDatabaseSwitchThread()", 1
        )[1].split("bool BeginSwitchDatabase", 1)[0]

        for owned in (
            "std::thread worker",
            "std::atomic<bool> done",
            "std::atomic<bool> success",
            "std::memory_order_release",
            "std::memory_order_acquire",
            "joinable()",
            ".join()",
        ):
            self.assertIn(owned, runtime)
        for removed in (
            "s_dbOpenThread",
            "s_dbOpenDone",
            "s_dbOpenSuccess",
        ):
            self.assertNotIn(removed, main)
        for delegated in (
            "g_databaseSwitchWorkerRuntime.Wait()",
            "g_databaseSwitchWorkerRuntime.PollFinished(success)",
            "g_databaseSwitchWorkerRuntime.Start(",
        ):
            self.assertIn(delegated, switch_body)

    def test_database_switch_blocking_operation_owns_order(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        operation = (ROOT / "src/DatabaseSwitchBlockingOperation.cpp").read_text(
            encoding="utf-8"
        )
        adapter = (ROOT / "src/DatabaseSwitchBlockingOperationAdapter.cpp").read_text(
            encoding="utf-8"
        )
        retained_in_order = (
            "SwitchDatabaseWorkerStage: stage=stop_batch_features",
            "environment.JoinBatchFeatures()",
            "environment.StopSameFolderScan()",
            "environment.StopSearch()",
            "environment.StopWaveform()",
            "environment.StopBackfill()",
            "environment.CloseDatabase()",
            "environment.OpenDatabase(request.newDbPathUtf8)",
            "environment.SetWaveformCacheMode(",
            "environment.EnqueueStartupReconciliation()",
            "SwitchDatabaseWorkerStage: stage=done",
            "return ok;",
        )
        positions = [operation.index(token) for token in retained_in_order]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("RunBlockingDatabaseSwitchOperation(", main)
        self.assertIn("sameFolderScanner_.stop();", adapter)
        self.assertNotIn("state.database.open(state.pendingDbPath, true)", main)

    def test_database_session_view_state_reset_owns_pure_ui_state(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src/DatabaseSessionViewStateReset.cpp").read_text(
            encoding="utf-8"
        )
        reset_body = self._function_body(
            main, "void ResetDatabaseState(AppState& state)"
        )

        for owned in (
            "state.searchResults.clear()",
            "state.clearFolderHierarchyState()",
            "state.playlists.clear()",
            "state.clearSelection()",
            "state.searchSuggestCandidates.clear()",
            "state.listTabs",
            "state.playHistory.clear()",
            "state.sameFolderRecycleBuffer.clear()",
        ):
            self.assertIn(owned, source)
            self.assertNotIn(owned, reset_body)
        for retained in (
            "state.searchWorker.stop()",
            "aura::database_session::ResetDatabaseSessionMediaCaches(state)",
            "aura::database_session::ResetDatabaseSessionScanState(state)",
            "glDeleteTextures",
            "StartDeferredSidebarBootstrap(state, true, true)",
        ):
            self.assertIn(retained, reset_body)
        for forbidden in (
            '#include "Database.h"',
            '#include "AsyncWorker.h"',
            '#include "CoverModule.h"',
            "glDeleteTextures",
            "Platform::",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, source)

    def test_database_session_scan_state_reset_owns_scan_presentation_state(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src/DatabaseSessionScanStateReset.cpp").read_text(
            encoding="utf-8"
        )
        reset_body = self._function_body(
            main, "void ResetDatabaseState(AppState& state)"
        )

        for owned in (
            "state.isScanning = false",
            "state.scanPhase.store(AppState::ScanPhase::Idle)",
            "state.scanProgress = 0",
            "state.scanAddedCount = 0",
            "state.clearScanTiming()",
            "state.clearScanWorkflowState()",
            "state.scanCancelRequested = false",
            'state.setScanFile("")',
            "state.clearScanErrors()",
            "state.autoFilterToScannedPath = false",
        ):
            self.assertIn(owned, source)
            self.assertNotIn(owned, reset_body)
        for retained in (
            "aura::database_session::ResetDatabaseSessionMediaCaches(state)",
            "glDeleteTextures",
            "g_miniWaveTextureCache.clear()",
            "StartDeferredSidebarBootstrap(state, true, true)",
        ):
            self.assertIn(retained, reset_body)
        for forbidden in (
            '#include "Database.h"',
            '#include "AsyncWorker.h"',
            '#include "ScanWorkflow.h"',
            '#include "CoverModule.h"',
            "glDeleteTextures",
            "Platform::",
            "resetScanReadyToWorkState",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, source)

    def test_database_session_media_cache_reset_owns_session_media_invalidation(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src/DatabaseSessionMediaCacheReset.cpp").read_text(
            encoding="utf-8"
        )
        reset_body = self._function_body(
            main, "void ResetDatabaseState(AppState& state)"
        )

        for owned in (
            "state.coverWallDatabaseInvalidationRequested = true",
            "state.invalidateCoverWallAfterDatabaseContentChange()",
            "state.preloadedCoverWallEntries.clear()",
            "state.preloadedCoverWallFullyDiscovered = false",
            "state.coverWall.reset()",
            "state.waveformCache.clear()",
            "state.clearPendingWaveforms()",
            "state.waveformFailed.clear()",
            "state.visibleFilePaths.clear()",
        ):
            self.assertIn(owned, source)
            self.assertNotIn(owned, reset_body)
        for retained in (
            "state.searchWorker.stop()",
            "state.waveformWorker.stop()",
            "aura::database_session::ResetDatabaseSessionScanState(state)",
            "glDeleteTextures",
            "state.spectrogramFilePath.clear()",
            "g_miniWaveTextureCache.clear()",
            "StartDeferredSidebarBootstrap(state, true, true)",
        ):
            self.assertIn(retained, reset_body)
        for forbidden in (
            '#include "Database.h"',
            '#include "AsyncWorker.h"',
            '#include "CoverModule.h"',
            "glDeleteTextures",
            "DeleteMiniWaveTexture",
            "Platform::",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, source)

    def test_waveform_rebuild_runtime_state_owns_reset_and_private_modes(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/WaveformRebuildRuntimeState.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/WaveformRebuildRuntimeState.cpp").read_text(
            encoding="utf-8"
        )
        recovery_adapter = (
            ROOT / "src/WaveformRebuildCancellationRecoveryStateAdapter.cpp"
        ).read_text(encoding="utf-8")

        self.assertEqual(main.count("g_waveformRebuildRuntimeState.Reset("), 4)
        self.assertEqual(main.count("g_waveformRebuildRuntimeState.Reset(state,"), 4)
        self.assertIn("runtimeState_.Reset(state_, false);", recovery_adapter)
        self.assertNotIn("ResetWaveformRebuildRuntimeState", main)
        self.assertNotIn("g_waveformRebuildBatchMiniOnly", main)
        self.assertNotIn("g_waveformRebuildIncrementalMiniOnly", main)
        self.assertIn("bool batchMiniOnly_ = false;", header)
        self.assertIn("bool incrementalMiniOnly_ = false;", header)
        self.assertIn("state.clearPendingWaveforms(cancelWorkerQueue);", source)
        for forbidden in (
            '#include "Database.h"',
            '#include "AsyncWorker.h"',
            '#include "CoverModule.h"',
            "Platform::",
            "glDelete",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_waveform_rebuild_batch_selector_owns_database_query_policy(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/WaveformRebuildBatchSelector.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/WaveformRebuildBatchSelector.cpp").read_text(
            encoding="utf-8"
        )
        lifecycle = (ROOT / "src/WaveformRebuildBatchLifecycle.cpp").read_text(
            encoding="utf-8"
        )
        pump = self._function_body(
            main, "static void PumpWaveformRebuild(AppState& state)"
        )

        self.assertEqual(main.count("SelectWaveformRebuildBatch("), 0)
        self.assertEqual(lifecycle.count("SelectWaveformRebuildBatch("), 1)
        for query in (
            "state.database.getAllFiles(",
            "state.database.getWaveformCacheMode()",
            "state.database.getFilesNeedingWaveformCache(",
            "state.database.getFilesNeedingDetailCache(",
            "state.database.getFilesByIds(",
        ):
            self.assertNotIn(query, pump)
        for owned in (
            "source.getAllFiles(request.limit, request.offset)",
            "source.getFilesNeedingWaveformCache(request.limit)",
            "source.getFilesNeedingDetailCache(request.limit)",
            "source.getFilesByIds(ids)",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState.h",
            "AsyncWorker.h",
            "DiagnosticsLogger",
            "ModuleUI.h",
            "Platform::",
            "glDelete",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_waveform_rebuild_failure_persistence_owns_previous_failure_policy(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/WaveformRebuildFailurePersistence.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/WaveformRebuildFailurePersistence.cpp").read_text(
            encoding="utf-8"
        )
        workflow = (ROOT / "src/WaveformRebuildRecordWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        pump = self._function_body(
            main, "static void PumpWaveformRebuild(AppState& state)"
        )

        self.assertEqual(main.count("PersistPreviousWaveformRebuildFailure("), 0)
        self.assertEqual(workflow.count("PersistPreviousWaveformRebuildFailure("), 1)
        for detail in (
            "WaveformExtractor::BuildCacheKey(",
            "state.database.upsertWaveformCacheStateByPath(",
            '"foreground_mini_rebuild_previous_failure"',
            '"foreground_waveform_rebuild_previous_failure"',
        ):
            self.assertNotIn(detail, pump)
        for owned in (
            "request.fullMode",
            "request.previouslyFailed",
            "sink.persistPreviousFailure(request.record, reason)",
            "WaveformExtractor::BuildCacheKey(",
            "database_.upsertWaveformCacheStateByPath(",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState.h",
            "AsyncWorker.h",
            "DiagnosticsLogger",
            "ModuleUI.h",
            "Platform::",
            "glDelete",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_waveform_rebuild_request_submission_owns_worker_routing(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/WaveformRebuildRequestSubmission.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/WaveformRebuildRequestSubmission.cpp").read_text(
            encoding="utf-8"
        )
        workflow = (ROOT / "src/WaveformRebuildRecordWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        selected_workflow = (
            ROOT / "src/SelectedWaveformRebuildWorkflow.cpp"
        ).read_text(encoding="utf-8")
        full_pump = self._function_body(
            main, "static void PumpWaveformRebuild(AppState& state)"
        )
        selected_pump = self._function_body(
            main, "static void PumpSelectedWaveformRebuild(AppState& state)"
        )

        self.assertEqual(main.count("SubmitWaveformRebuildRequest("), 0)
        self.assertEqual(workflow.count("SubmitWaveformRebuildRequest("), 1)
        self.assertEqual(
            selected_workflow.count("SubmitWaveformRebuildRequest("), 1
        )
        for body in (full_pump, selected_pump):
            self.assertNotIn("state.waveformWorker.submitVisibleRequest(", body)
            self.assertNotIn("state.waveformWorker.submitDetailRequest(", body)
            self.assertNotIn("SubmitWaveformRebuildRequest(", body)
        for owned in (
            "request.miniOnly",
            "port.submitMini(request.filePath)",
            "port.submitDetail(request.filePath, request.fileId)",
            "worker_.submitVisibleRequest(filePath)",
            "worker_.submitDetailRequest(filePath, fileId)",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState.h",
            "Database.h",
            "DiagnosticsLogger",
            "ModuleUI.h",
            "Platform::",
            "glDelete",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_adaptive_mini_format_finalization_owns_terminal_transitions(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/AdaptiveMiniWaveformFormatFinalization.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/AdaptiveMiniWaveformFormatFinalization.cpp").read_text(
            encoding="utf-8"
        )
        lifecycle = (ROOT / "src/WaveformRebuildBatchLifecycle.cpp").read_text(
            encoding="utf-8"
        )
        recovery = (
            ROOT / "src/WaveformRebuildCancellationRecovery.cpp"
        ).read_text(encoding="utf-8")
        start = self._function_body(
            main, "static void MaybeStartAdaptiveMiniWaveformFormatUpgrade(AppState& state)"
        )
        pump = self._function_body(
            main, "static void PumpWaveformRebuild(AppState& state)"
        )

        self.assertEqual(main.count("FinalizeAdaptiveMiniWaveformFormat("), 1)
        self.assertEqual(lifecycle.count("FinalizeAdaptiveMiniWaveformFormat("), 1)
        self.assertEqual(recovery.count("FinalizeAdaptiveMiniWaveformFormat("), 1)
        self.assertNotIn(
            "state.database.markAdaptiveMiniWaveformFormatUpgradeComplete()", start
        )
        self.assertNotIn(
            "state.database.markAdaptiveMiniWaveformFormatUpgradeComplete()", pump
        )
        for owned in (
            "AdaptiveMiniFormatFinalizationReason::NoWork",
            "AdaptiveMiniFormatFinalizationReason::RebuildCompleted",
            "AdaptiveMiniFormatFinalizationReason::Cancelled",
            "port.markUpgradeComplete()",
            "database_.markAdaptiveMiniWaveformFormatUpgradeComplete()",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState.h",
            "AsyncWorker.h",
            "DiagnosticsLogger",
            "ModuleUI.h",
            "Platform::",
            "glDelete",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_adaptive_mini_format_preparation_owns_database_decision(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/AdaptiveMiniWaveformFormatPreparation.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/AdaptiveMiniWaveformFormatPreparation.cpp").read_text(
            encoding="utf-8"
        )
        start = self._function_body(
            main, "static void MaybeStartAdaptiveMiniWaveformFormatUpgrade(AppState& state)"
        )

        self.assertEqual(main.count("PrepareAdaptiveMiniWaveformFormat("), 1)
        for removed in (
            "state.database.usesAdaptiveMiniWaveform()",
            "state.database.purgeAllMiniWaveform()",
            "state.database.countFilesNeedingWaveformCache()",
        ):
            self.assertNotIn(removed, start)
        for owned in (
            "source.usesAdaptiveMiniWaveform()",
            "source.purgeAllMiniWaveform()",
            "source.countFilesNeedingWaveformCache()",
            "database_.usesAdaptiveMiniWaveform()",
            "database_.purgeAllMiniWaveform()",
            "database_.countFilesNeedingWaveformCache()",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState.h",
            "AsyncWorker.h",
            "DiagnosticsLogger",
            "ModuleUI.h",
            "Platform::",
            "glDelete",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_adaptive_mini_rebuild_activation_owns_success_transition(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/AdaptiveMiniWaveformRebuildActivation.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/AdaptiveMiniWaveformRebuildActivation.cpp").read_text(
            encoding="utf-8"
        )
        start = self._function_body(
            main, "static void MaybeStartAdaptiveMiniWaveformFormatUpgrade(AppState& state)"
        )

        self.assertEqual(main.count("ActivateAdaptiveMiniWaveformRebuild("), 1)
        for removed in (
            "state.waveformRebuildMiniOnlyMode = true",
            "g_waveformRebuildRuntimeState.setIncrementalMiniOnly(true)",
            "state.adaptiveMiniWaveformFormatRebuildPending = false",
            "state.adaptiveMiniWaveformFormatStampPending = true",
        ):
            self.assertNotIn(removed, start)
        for owned in (
            "if (!startSucceeded)",
            "state.waveformRebuildMiniOnlyMode = true",
            "state.incrementalMiniOnly = true",
            "state.rebuildPending = false",
            "state.stampPending = true",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState.h",
            "AsyncWorker.h",
            "Database.h",
            "DiagnosticsLogger",
            "ModuleUI.h",
            "Platform::",
            "WaveformRebuildRuntimeState.h",
            "glDelete",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_waveform_rebuild_pump_gate_owns_admission_policy(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/WaveformRebuildPumpGate.h").read_text(encoding="utf-8")
        source = (ROOT / "src/WaveformRebuildPumpGate.cpp").read_text(encoding="utf-8")
        pump = self._function_body(main, "static void PumpWaveformRebuild(AppState& state)")

        self.assertEqual(main.count("EvaluateWaveformRebuildCapacityGate("), 1)
        self.assertEqual(main.count("ShouldSampleWaveformRebuildDrain("), 1)
        self.assertEqual(main.count("EvaluateWaveformRebuildBatchGate("), 1)
        prefix = pump.split("LoadWaveformRebuildBatchLifecycle(", 1)[0]
        self.assertEqual(prefix.count("state.waveformWorker.pendingCount()"), 2)
        for removed in (
            "size_t pending = state.waveformUrgentPending.size()",
            "pending >= WAVEFORM_REBUILD_QUEUE_THRESHOLD",
            "const bool alreadySubmittedRebuildWork",
        ):
            self.assertNotIn(removed, pump)
        for owned in (
            "input.urgentPending + input.normalPending + input.workerPending",
            "pending >= input.queueThreshold",
            "input.batchIndex < static_cast<int>(input.batchSize)",
            "input.progress > 0 || input.offset > 0",
            "WaveformRebuildBatchGateDecision::WaitForDrain",
            "WaveformRebuildBatchGateDecision::LoadBatch",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState.h",
            "AsyncWorker.h",
            "Database.h",
            "DiagnosticsLogger",
            "ModuleUI.h",
            "Platform::",
            "WaveformRebuildRuntimeState.h",
            "glDelete",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_waveform_rebuild_record_workflow_owns_record_transition(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/WaveformRebuildRecordWorkflow.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/WaveformRebuildRecordWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        pump = self._function_body(main, "static void PumpWaveformRebuild(AppState& state)")

        self.assertEqual(pump.count("ProcessNextWaveformRebuildRecord("), 1)
        for removed in (
            "waveformRebuildQueuedIds.count(",
            "waveformRebuildQueuedIds.insert(",
            "PersistPreviousWaveformRebuildFailure(",
            "SubmitWaveformRebuildRequest(",
            "waveformRebuildProgress++",
            "waveformRebuildFailed++",
            "waveformRebuildSkipped++",
        ):
            self.assertNotIn(removed, pump)
        for owned in (
            "queuedIds.insert(record.id).second",
            "++result.state.batchIndex",
            "++result.state.offset",
            "++result.state.progress",
            "++result.state.failed",
            "++result.state.pending",
            "++result.state.skipped",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState",
            "main.cpp",
            "WindowsMain",
            "platform/",
            "ModuleUI",
            "UIRenderer",
            "DiagnosticsLogger",
            "DIAG_",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_waveform_rebuild_batch_lifecycle_owns_load_transaction(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/WaveformRebuildBatchLifecycle.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/WaveformRebuildBatchLifecycle.cpp").read_text(
            encoding="utf-8"
        )
        pump = self._function_body(main, "static void PumpWaveformRebuild(AppState& state)")

        self.assertEqual(pump.count("LoadWaveformRebuildBatchLifecycle("), 1)
        for removed in (
            "waveformRebuildQueuedIds.clear()",
            "SelectWaveformRebuildBatch(",
            "AdaptiveMiniFormatFinalizationReason::RebuildCompleted",
        ):
            self.assertNotIn(removed, pump)
        for owned in (
            "queuedIds.clear()",
            "SelectWaveformRebuildBatch(source, request.batch)",
            "FinalizeAdaptiveMiniWaveformFormat(",
            "AdaptiveMiniFormatFinalizationReason::RebuildCompleted",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState",
            "main.cpp",
            "WindowsMain",
            "platform/",
            "DiagnosticsLogger",
            "DIAG_",
            "WaveformRebuildRuntimeState",
            "ModuleUI",
            "UIRenderer",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_waveform_rebuild_cancellation_recovery_owns_cancel_transaction(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (
            ROOT / "src/WaveformRebuildCancellationRecovery.h"
        ).read_text(encoding="utf-8")
        source = (
            ROOT / "src/WaveformRebuildCancellationRecovery.cpp"
        ).read_text(encoding="utf-8")
        adapter = (
            ROOT / "src/WaveformRebuildCancellationRecoveryStateAdapter.cpp"
        ).read_text(encoding="utf-8")
        pump = self._function_body(main, "static void PumpWaveformRebuild(AppState& state)")
        cancel = pump.split("if (state.waveformRebuildCancel)", 1)[1].split(
            "const int capacityWorkerPending", 1
        )[0]

        self.assertEqual(cancel.count("recoveryAdapter.recover("), 1)
        self.assertNotIn("RecoverCancelledWaveformRebuild(", cancel)
        self.assertEqual(adapter.count("RecoverCancelledWaveformRebuild("), 1)
        for removed in (
            "FinalizeAdaptiveMiniWaveformFormat(",
            "RestartWaveformWorkerForForegroundRebuild(state)",
            "g_waveformRebuildRuntimeState.Reset(state, false)",
        ):
            self.assertNotIn(removed, cancel)
        finalization = source.index("FinalizeAdaptiveMiniWaveformFormat(")
        apply_state = source.index("effects.applyAdaptiveState(result.state)")
        restart = source.index("effects.restartForegroundWorker()")
        reset = source.index("effects.resetRuntimeState()")
        self.assertLess(finalization, apply_state)
        self.assertLess(apply_state, restart)
        self.assertLess(restart, reset)
        for forbidden in (
            "AppState",
            "Database",
            "AsyncWorker",
            "WaveformWorker",
            "WaveformRebuildRuntimeState",
            "DiagnosticsLogger",
            "DIAG_",
            "ModuleUI",
            "UIRenderer",
            "platform/",
            "main.cpp",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_scan_waveform_backfill_drain_owns_state_machine(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/ScanWaveformBackfillDrainWorkflow.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/ScanWaveformBackfillDrainWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        pump = self._function_body(
            main, "static void PumpScanWaveformBackfillQueue(AppState& state)"
        )

        self.assertEqual(pump.count("DrainScanWaveformBackfillQueue("), 1)
        self.assertIn("AURA_ENABLE_WAVEFORM_BACKFILL_DRAIN", pump)
        for removed in (
            "backfillQueue.snapshot()",
            "backfillQueue.tryDequeueKind(",
            "database.getFilesByIds(",
            "backfillQueue.reportOutcome(",
            "waveformWorker.submitBackfillDetailRequest(",
        ):
            self.assertNotIn(removed, pump)
        for owned in (
            "observations.admissionScanning()",
            "observations.reconcileScanRunning()",
            "queue.queuedWaveformCount()",
            "observations.batchScanning()",
            "queue.tryDequeueWaveform(job)",
            'PersistPermanentFailure(records, job, "missing_audio_row")',
            '"waveform_submit_failed"',
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState",
            "Database",
            "AsyncWorker",
            "WaveformWorker",
            "DiagnosticsLogger",
            "DIAG_",
            "ModuleUI",
            "UIRenderer",
            "platform/",
            "main.cpp",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_selected_waveform_rebuild_workflow_owns_state_machine(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/SelectedWaveformRebuildWorkflow.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/SelectedWaveformRebuildWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        pump = self._function_body(
            main, "static void PumpSelectedWaveformRebuild(AppState& state)"
        )

        self.assertEqual(
            pump.count("g_selectedWaveformRebuildWorkflow.pump("), 1
        )
        for removed in (
            "beginTransaction(",
            "clearWaveformCacheByPath(",
            "commitTransaction(",
            "rollbackTransaction(",
            "SubmitWaveformRebuildRequest(",
            "addPendingWaveform(",
            "forceRegenerateWaveform(",
        ):
            self.assertNotIn(removed, pump)
        for removed_global in (
            "g_selectedWaveformRebuildQueue",
            "g_selectedWaveformRebuildCleared",
            "g_selectedWaveformRebuildQueued",
            "g_selectedWaveformRebuildPending",
            "SELECTED_WAVEFORM_REBUILD_PER_FRAME",
        ):
            self.assertNotIn(removed_global, main)
        for owned in (
            "constexpr std::size_t kPerFrame = 4",
            "queue_.pump(kPerFrame",
            "storage.beginTransaction()",
            "storage.clearWaveformCacheByPath(item.filePath)",
            "effects.loadedFilePath()",
            "storage.miniOnlyPolicy()",
            "SubmitWaveformRebuildRequest(",
            "effects.addPendingWaveform(item.filePath, !miniOnly)",
            "effects.forceLoadedItem(item)",
            "effects.reportCompleted(result.stats)",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState",
            "Database.h",
            "AudioEngine",
            "WaveformWorker",
            "DiagnosticsLogger",
            "DIAG_",
            "ModuleUI",
            "UIRenderer",
            "ImGui",
            "platform/",
            "main.cpp",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_selected_record_rescan_workflow_owns_domain_policy(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/SelectedRecordRescanWorkflow.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/SelectedRecordRescanWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        batch = self._function_body(
            main,
            "static void RescanSelectedFiles(AppState& state, "
            "const std::vector<AudioRecord>& records)",
        )

        self.assertEqual(
            batch.count("aura::record_rescan::RescanSelectedRecords("), 1
        )
        for moved in (
            "MetadataReader::readMetadata",
            "ProbeRegularFile",
            "probeAudioFileInfo",
            "getWaveformCacheStateByPath",
            "clearWaveformCacheByPath",
            "updateFile(",
            "beginTransaction(",
            "commitTransaction(",
        ):
            self.assertNotIn(moved, batch)
        for owned in (
            "ApplyMetadata(update, metadata)",
            "port.wasDecoderInitFailure(item.filePath)",
            "port.probeAudio(item.filePath, audio)",
            "port.lastWriteTimeUnixSeconds(item.filePath)",
            "port.updateRecord(item, update)",
            "port.clearWaveformCache(item.filePath)",
            "port.beginTransaction()",
            "port.processProtected(item)",
            "port.commitTransaction()",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState",
            "Database.h",
            "MetadataReader",
            "DiagnosticsLogger",
            "DIAG_",
            "ModuleUI",
            "UIRenderer",
            "ImGui",
            "Platform::",
            "platform/",
            "main.cpp",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_bpm_batch_scan_workflow_owns_startup_policy(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/BpmBatchScanWorkflow.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/BpmBatchScanWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        startup = main.split(
            "static bool StartBpmBatchScan(AppState& state, "
            "const std::string& folderPathPrefix = {}) {",
            1,
        )[1].split("\n}", 1)[0]

        self.assertEqual(
            startup.count("aura::music_analysis::StartBpmBatchScan("), 1
        )
        for moved in (
            "queryMissingBpm(",
            "cancelByPriority(",
            "bpmBatchTotal.store(",
            "bpmBatchRunning.store(",
            "MusicAnalysisRequest request",
            "musicAnalyzer.submit(",
        ):
            self.assertNotIn(moved, startup)
        for owned in (
            "port.databaseOpen()",
            "port.queryMissingBpm(50000, true, folderPathPrefix)",
            "port.cancelP2()",
            "port.setBatchTotal(static_cast<int>(rows.size()))",
            "port.nextGeneration()",
            "port.submitP2(request)",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState",
            "Database",
            "MusicAnalyzer",
            "MusicAnalysisRequest",
            "AnalysisPriority",
            "DiagnosticsLogger",
            "DIAG_",
            "ModuleUI",
            "UIRenderer",
            "ImGui",
            "Platform::",
            "platform/",
            "main.cpp",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_bpm_batch_result_workflow_owns_p2_result_policy(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/BpmBatchResultWorkflow.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/BpmBatchResultWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        batch_prefix = main.split("for (const auto& res : bpmResults) {", 1)[1].split(
            "ProcessSingleMusicAnalysisResult(", 1
        )[0]

        self.assertEqual(batch_prefix.count("ProcessBpmBatchResult("), 1)
        for moved in (
            "res.priority == AnalysisPriority::P2",
            "m_batchDone.fetch_add",
            "setMusicBpm(",
            "m_batchFound.fetch_add",
            "setMusicKey(",
            "setMusicMood(",
            "snprintf(",
        ):
            self.assertNotIn(moved, batch_prefix)
        for owned in (
            "if (!result.isP2 || result.fileId < 0) return false",
            "progress.incrementDone()",
            "persistence.persistBpm(result.fileId, -1.0f, 0.0f)",
            "result.bpm.bpm > 40.0f && result.bpm.bpm < 240.0f",
            "progress.incrementFound()",
            "persistence.persistKey(",
            "persistence.persistMood(",
            "view.applyToFirstMatchingRecord(",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState",
            "Database",
            "MusicAnalyzer",
            "MusicAnalysisResult",
            "AnalysisPriority",
            "MusicFieldSource",
            "AudioRecord",
            "ImGui",
            "DiagnosticsLogger",
            "DIAG_",
            "Platform::",
            "platform/",
            "main.cpp",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_single_music_result_workflow_owns_ordinary_result_policy(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/SingleMusicAnalysisResultWorkflow.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/SingleMusicAnalysisResultWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        ordinary = main.split("ProcessBpmBatchResult(", 1)[1].split(
            "// Auto-dismiss bubble", 1
        )[0]

        self.assertEqual(ordinary.count("ProcessSingleMusicAnalysisResult("), 1)
        for moved in (
            "res.generation != state.bpmGeneration",
            "pendingBpm.isAnalyzing = false",
            "setMusicKey(",
            "setMusicMood(",
            "pendingBpm.bpm",
            "beatGridOffset",
            "pendingBpm.failed",
        ):
            self.assertNotIn(moved, ordinary)
        for owned in (
            "if (result.generation != currentGeneration) return false",
            "state.markAnalysisComplete()",
            "persistence.persistKey(",
            "view.applyKeyToFirstMatchingRecord(",
            "persistence.persistMood(",
            "update.applyAiAutoSource = true",
            "update.applyBeatOffset = true",
            "update.applyFailed = true",
            "state.applyPendingUpdate(update)",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState", "Database", "MusicAnalyzer", "AnalysisPriority",
            "MusicFieldSource", "AudioRecord", "ImGui", "DiagnosticsLogger",
            "DIAG_", "Platform::", "platform/", "main.cpp", "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_bpm_batch_completion_workflow_owns_drain_transition(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/BpmBatchCompletionWorkflow.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/BpmBatchCompletionWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        frame = main.split("// Mark T3 batch complete when queue drains", 1)[1].split(
            "// Deferred batch cover import", 1
        )[0]

        self.assertEqual(frame.count("CompleteBpmBatchIfDrained("), 1)
        for moved in (
            "batchRunning()",
            "pendingCount() == 0",
            "isAnalyzing()",
            "m_batchRunning.store(false)",
            "browserMetaCacheDir.clear()",
        ):
            self.assertNotIn(moved, frame)
        for owned in (
            "!snapshot.batchRunning",
            "snapshot.pendingCount != 0",
            "snapshot.analyzing",
            "port.markBatchStopped()",
            "port.invalidateBrowserMetadataCache()",
        ):
            self.assertIn(owned, source)
        self.assertLess(
            source.index("port.markBatchStopped()"),
            source.index("port.invalidateBrowserMetadataCache()"),
        )
        for forbidden in (
            "AppState", "Database", "MusicAnalyzer", "atomic", "ImGui",
            "DiagnosticsLogger", "DIAG_", "Platform::", "platform/",
            "main.cpp", "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_cover_batch_operation_workflow_owns_transaction_policy(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/CoverBatchOperationWorkflow.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/CoverBatchOperationWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        pump = main.split("static void PumpCoverBatchOperation(AppState& state)", 1)[
            1
        ].split("// Explicit physical embedding", 1)[0]

        self.assertEqual(
            pump.count("aura::cover_batch::PumpCoverBatchOperation("), 1
        )
        for moved in (
            "XXH3_64bits", "Hex64Local", "hasCoverThumb(",
            "assignEmbeddedCoverByPaths(", "clearCoverByPaths(",
            "searchResults", "InvalidateFileCoverCaches(",
            "coverBatchActive = false", "coverBatchTargets.clear()",
        ):
            self.assertNotIn(moved, pump)
        for owned in (
            "if (!input.active) return false",
            "input.progress == 0 && input.total > 0",
            "persistence.currentUnixTime()",
            "ResolveCoverImageMime(input.imageData, input.mimeHint)",
            "BuildEmbeddedCoverKey(input.imageData, mime)",
            "input.imageData.size() <= 512U * 1024U",
            "persistence.assignEmbeddedCoverByPaths(",
            "persistence.clearCoverByPaths(input.targets)",
            "view.applyVisibleRows(",
            "view.invalidateFileCoverCaches(input.targets)",
            "view.clearFolderCoverNegativeCache()",
            "view.requestCoverWallRefresh()",
            "view.syncToTab()",
            "state.finishBatch(input.total)",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState", "Database", "AudioRecord", "CoverModule", "ImGui",
            "Localization", "Platform::", "platform/", "main.cpp",
            "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_embed_metadata_batch_runtime_owns_future_lifecycle(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/EmbedMetadataBatchRuntime.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/EmbedMetadataBatchRuntime.cpp").read_text(
            encoding="utf-8"
        )
        pump = main.split("static void PumpEmbedMetadataBatch(AppState& state)", 1)[
            1
        ].split("// ", 1)[0]

        self.assertEqual(pump.count("runtime.pump("), 1)
        for moved in (
            "static std::future", "static bool", "wait_for(",
            "future_status::ready", "embedBatchProgress++",
            "embedBatchActive = false", "embedBatchTargets.clear()",
        ):
            self.assertNotIn(moved, pump)
        for owned in (
            "if (!state.active()) return",
            "state.progress() < state.total()",
            "tasks.launch(state.currentTask())",
            "wait_for(std::chrono::seconds(0))",
            "future_.get()", "state.advance()", "state.finish()",
        ):
            self.assertIn(owned, source)
        for forbidden in (
            "AppState", "AudioRecord", "Database", "MetadataWriter", "ImGui",
            "Platform::", "platform/", "main.cpp", "WindowsMain",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_cover_rebuild_record_processor_owns_record_policy(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/CoverRebuildRecordProcessor.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/CoverRebuildRecordProcessor.cpp").read_text(
            encoding="utf-8"
        )
        pump = main.split("static void PumpCoverRebuild(AppState& state)", 1)[
            1
        ].split("// --- Rebuild all waveform caches", 1)[0]
        self.assertEqual(
            pump.count("aura::cover_rebuild::ProcessCoverRebuildRecord("), 1
        )
        for moved in (
            "TryGetFolderCoverKeyFast(", "GetFolderCoverImagePath(",
            "NormalizeCoverKeyByVisualIdentity(", "hasEmbeddedCover =",
            "hasAnyCover =", "coverRebuildFound++",
        ):
            self.assertNotIn(moved, pump)
        for forbidden in (
            "AppState", "AudioRecord", "Database", "CoverModule", "ImGui",
            "main.cpp", "WindowsMain", "platform/",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_cover_rebuild_lifecycle_workflow_owns_terminal_order(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/CoverRebuildLifecycleWorkflow.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/CoverRebuildLifecycleWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        pump = main.split("static void PumpCoverRebuild(AppState& state)", 1)[
            1
        ].split("// --- Rebuild all waveform caches", 1)[0]
        self.assertEqual(
            pump.count("aura::cover_rebuild::ResolveCoverRebuildLifecycle("), 2
        )
        for moved in (
            "state.coverRebuildActive = false", "state.coverRebuildBatch.clear()",
            "state.currentCoverFacets.clear()", "ForceCoverWallReload(state)",
            "RefreshSearchResults(state, true)",
        ):
            self.assertNotIn(moved, pump)
        self.assertIn("if (input.cancelRequested)", source)
        self.assertIn("if (!input.batchEmpty) return false", source)
        self.assertIn("if (input.coverRegistryStale)", source)
        for forbidden in (
            "AppState", "Database", "CoverModule", "ImGui", "main.cpp",
            "WindowsMain", "platform/",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_daw_running_probe_uses_thread_priority_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        probe_body = source.split(
            "static DawRunningProbeResults ProbeDawRunningStateSnapshots",
            1,
        )[1].split("static void", 1)[0]
        self.assertIn("Platform::SetThreadBackgroundPriority()", probe_body)
        self.assertNotIn("SetThreadPriority(", probe_body)

    def test_cover_rebuild_batch_commit_owns_database_batch_order(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/CoverRebuildBatchCommitWorkflow.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/CoverRebuildBatchCommitWorkflow.cpp").read_text(
            encoding="utf-8"
        )
        pump = main.split("static void PumpCoverRebuild(AppState& state)", 1)[
            1
        ].split("// --- Rebuild all waveform caches", 1)[0]
        self.assertEqual(
            pump.count("aura::cover_rebuild::CommitCoverRebuildBatch("), 1
        )
        for moved in (
            "updateHasCoverByPathsBatch(",
            "updateCoverKeyCacheBatch(",
        ):
            self.assertNotIn(moved, pump)
        self.assertLess(
            source.index("persistence.setHasCover"),
            source.index("persistence.clearHasCover"),
        )
        self.assertLess(
            source.index("persistence.clearHasCover"),
            source.index("persistence.updateCoverKeyCache"),
        )
        for forbidden in (
            "AppState", "Database", "AudioRecord", "CoverModule", "ImGui",
            "main.cpp", "WindowsMain", "platform/",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_public_platform_header_cannot_leak_native_type(self) -> None:
        findings = self._scan({"src/platform/system.h": "struct System { HANDLE handle; };\n"})
        self.assertIn("public-native-type", self._rules(findings))

    def test_elevated_native_lifecycle_is_private_and_reaper_is_non_admin(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(
            encoding="utf-8"
        )
        installer = (ROOT / "src" / "DawBridgeInstaller.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("RunElevatedProcessAndWait", header)
        self.assertNotIn("HANDLE", header)
        self.assertIn("RunElevatedProcessAndWait", win32)
        for token in (
            "ShellExecuteExW",
            "WaitForSingleObject",
            "TerminateProcess",
            "CloseHandle",
        ):
            self.assertIn(token, win32)
            self.assertNotIn(token, installer)
        reaper = re.search(
            r"static Installer MakeReaperExtensionInstaller\(\)\s*\{(?P<body>.*?)^\}",
            installer,
            re.MULTILINE | re.DOTALL,
        )
        self.assertIsNotNone(reaper, "MakeReaperExtensionInstaller block is missing")
        self.assertRegex(
            reaper.group("body"),
            r"(?m)^\s*inst\.requiresAdmin\s*=\s*false;\s*$",
            "current REAPER registry entry must remain non-admin",
        )

    def test_文件监听公共头不暴露原生类型(self) -> None:
        legacy_header = ROOT / "src" / "FileSystemWatcher.h"
        platform_header = ROOT / "src" / "platform" / "file_watcher.h"
        self.assertFalse(legacy_header.exists(), "旧业务目录仍持有文件监听公共头")
        self.assertTrue(platform_header.is_file(), "src/platform 缺少文件监听公共头")
        header = platform_header.read_text(encoding="utf-8")
        for native_include in ("windows.h", "fileapi.h", "winbase.h"):
            self.assertNotIn(
                native_include,
                header,
                f"FileSystemWatcher 公共头仍直接包含 {native_include}",
            )
        for native_type in ("HANDLE", "OVERLAPPED", "INVALID_HANDLE_VALUE"):
            self.assertNotIn(
                native_type,
                header,
                f"FileSystemWatcher 公共头仍暴露原生类型 {native_type}",
            )
        self.assertIn("struct WatchInfo;", header)

    def test_文件监听实现归属平台目录(self) -> None:
        legacy_source = ROOT / "src" / "FileSystemWatcher.cpp"
        platform_source = ROOT / "src" / "platform" / "file_watcher.cpp"
        self.assertFalse(legacy_source.exists(), "旧业务目录仍持有文件监听原生实现")
        self.assertTrue(platform_source.is_file(), "src/platform 缺少文件监听实现")
        cmake_text = "\n".join(
            (ROOT / path).read_text(encoding="utf-8")
            for path in ("CMakeLists.txt", "cmake/harken_search_behavior_tests.cmake")
        )
        self.assertNotIn("src/FileSystemWatcher.cpp", cmake_text)
        self.assertIn("src/platform/file_watcher.cpp", cmake_text)

    def test_系统服务拥有独立公共头(self) -> None:
        system_header_path = ROOT / "src" / "platform" / "system.h"
        self.assertTrue(system_header_path.is_file(), "src/platform 缺少 system.h")
        system_header = system_header_path.read_text(encoding="utf-8")
        compatibility_header = (ROOT / "src" / "platform" / "platform.h").read_text(
            encoding="utf-8"
        )
        declarations = (
            "bool IsSystemDarkMode();",
            "std::string GetMachineId();",
            "void OpenInFileManager(const std::string& pathUtf8);",
            "void OpenUrl(const std::string& url);",
            "void InitThreadCom();",
            "void UninitThreadCom();",
            "void SetThreadBackgroundPriority();",
            "std::string GetAppDataDir();",
            "std::string GetExeDir();",
        )
        for declaration in declarations:
            self.assertIn(declaration, system_header)
            self.assertNotIn(declaration, compatibility_header)
        self.assertNotIn('#include "system.h"', compatibility_header)

    def test_系统服务调用方直接包含专用头(self) -> None:
        consumers = (
            "src/HarkenSearchIcons.cpp",
            "src/CoverModule.cpp",
            "src/ProductEdition.cpp",
            "src/Theme.cpp",
            "src/Utils.cpp",
            "src/main.cpp",
            "tests/test_cover_runtime_stubs.cpp",
        )
        for relative_path in consumers:
            source = (ROOT / relative_path).read_text(encoding="utf-8")
            self.assertRegex(
                source,
                r'#include "(?:src/)?platform/system\.h"',
                f"{relative_path} 未直接包含系统服务专用头",
            )

    def test_win32_系统集成实现归属专用源文件(self) -> None:
        system_source_path = ROOT / "src" / "platform" / "system_win32.cpp"
        self.assertTrue(system_source_path.is_file(), "src/platform 缺少 system_win32.cpp")
        system_source = system_source_path.read_text(encoding="utf-8")
        legacy_source = (ROOT / "src" / "platform" / "platform_win32.cpp").read_text(
            encoding="utf-8"
        )
        definitions = (
            "bool IsSystemDarkMode()",
            "std::string GetMachineId()",
            "void OpenInFileManager(const std::string& pathUtf8)",
            "void OpenUrl(const std::string& url)",
            "bool LaunchProcess(const std::string& exePathUtf8, const std::string& argsUtf8)",
        )
        for definition in definitions:
            self.assertIn(definition, system_source)
            self.assertNotIn(definition, legacy_source)
        cmake_text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("src/platform/system_win32.cpp", cmake_text)

    def test_win32_系统生命周期与对话框实现归属专用源文件(self) -> None:
        system_source = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(
            encoding="utf-8"
        )
        legacy_source = (ROOT / "src" / "platform" / "platform_win32.cpp").read_text(
            encoding="utf-8"
        )
        definitions = (
            "void InitThreadCom()",
            "void UninitThreadCom()",
            "void SetThreadBackgroundPriority()",
            "std::string OpenFileDialog(void* ownerWindow",
            "std::string SaveFileDialog(void* ownerWindow",
            "std::string BrowseFolderDialog(void* ownerWindow",
            "std::string GetAppDataDir()",
            "std::string GetExeDir()",
        )
        for definition in definitions:
            self.assertIn(definition, system_source)
            self.assertNotIn(definition, legacy_source)

    def test_macos_系统集成实现归属专用源文件(self) -> None:
        system_source_path = ROOT / "src" / "platform" / "system_macos.mm"
        self.assertTrue(system_source_path.is_file(), "src/platform 缺少 system_macos.mm")
        system_source = system_source_path.read_text(encoding="utf-8")
        legacy_source = (ROOT / "src" / "platform" / "platform_macos.mm").read_text(
            encoding="utf-8"
        )
        definitions = (
            "bool IsSystemDarkMode()",
            "std::string GetMachineId()",
            "void OpenInFileManager(const std::string& pathUtf8)",
            "void OpenUrl(const std::string& url)",
            "bool LaunchProcess(const std::string& exePathUtf8, const std::string& argsUtf8)",
        )
        for definition in definitions:
            self.assertIn(definition, system_source)
            self.assertNotIn(definition, legacy_source)
        cmake_text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("src/platform/system_macos.mm", cmake_text)

    def test_macos_系统生命周期与对话框实现归属专用源文件(self) -> None:
        system_source = (ROOT / "src" / "platform" / "system_macos.mm").read_text(
            encoding="utf-8"
        )
        legacy_source = (ROOT / "src" / "platform" / "platform_macos.mm").read_text(
            encoding="utf-8"
        )
        definitions = (
            "void InitThreadCom()",
            "void UninitThreadCom()",
            "void SetThreadBackgroundPriority()",
            "std::string OpenFileDialog(void* /*ownerWindow*/",
            "std::string SaveFileDialog(void* /*ownerWindow*/",
            "std::string BrowseFolderDialog(void* /*ownerWindow*/",
            "std::string GetAppDataDir()",
            "std::string GetExeDir()",
        )
        for definition in definitions:
            self.assertIn(definition, system_source)
            self.assertNotIn(definition, legacy_source)

    def test_folder_dialog_pal_preserves_optional_initial_path(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(
            encoding="utf-8"
        )
        macos = (ROOT / "src" / "platform" / "system_macos.mm").read_text(
            encoding="utf-8"
        )

        self.assertIn('const std::string& initialPathUtf8 = ""', header)
        self.assertIn("if (initialPathUtf8.empty())", win32)
        self.assertIn("return BrowseFolderDialogLegacy(ownerWindow, title);", win32)
        self.assertIn(
            "return BrowseFolderDialogModernWithInitialPath(ownerWindow, initialPathUtf8);", win32
        )

        legacy = win32.split("std::string BrowseFolderDialogLegacy(", 1)[1].split(
            "std::string BrowseFolderDialogModernWithInitialPath(", 1
        )[0]
        self.assertIn("SHBrowseForFolderW", legacy)
        self.assertIn("MAX_PATH", legacy)

        modern = win32.split("std::string BrowseFolderDialogModernWithInitialPath(", 1)[1].split(
            "std::string BrowseFolderDialog(void* ownerWindow", 1
        )[0]
        for required in (
            "IFileDialog",
            "FOS_PICKFOLDERS",
            "FOS_NOCHANGEDIR",
            "FOS_FORCEFILESYSTEM",
            "SHCreateItemFromParsingName",
            "SetFolder",
            "TextEncoding::Utf8ToWide(initialPathUtf8)",
            "TextEncoding::WideToUtf8(wpath)",
            "CoInitializeEx",
            "CoUninitialize",
        ):
            self.assertIn(required, modern)
        self.assertNotIn("SHBrowseForFolderW", modern)
        self.assertNotIn("MAX_PATH", modern)
        self.assertIn("fileURLWithPath", macos)
        self.assertIn("setDirectoryURL", macos)

    def test_main_folder_dialog_uses_modern_system_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
        win32 = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")

        self.assertIn(
            "std::string BrowseFolderDialogModern(void* ownerWindow, const std::string& title);",
            header,
        )
        body = main.split("std::string OpenFolderDialog(HWND hwnd)", 1)[1].split(
            "// Application State", 1
        )[0]
        self.assertIn(
            'return Platform::BrowseFolderDialogModern(hwnd, u8"选择音频文件夹");',
            body,
        )
        for native_token in (
            "CoInitializeEx",
            "CoCreateInstance",
            "CoUninitialize",
            "IFileDialog",
            "IShellItem",
            "FOS_PICKFOLDERS",
        ):
            self.assertNotIn(native_token, body)

        modern = win32.split(
            "std::string BrowseFolderDialogModern(void* ownerWindow, const std::string& title)",
            1,
        )[1].split("std::string GetAppDataDir()", 1)[0]
        self.assertIn("FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM", modern)
        self.assertNotIn("FOS_NOCHANGEDIR", modern)
        self.assertIn("dialog->SetTitle(wTitle.c_str())", modern)
        self.assertIn("dialog->Show(static_cast<HWND>(ownerWindow))", modern)

    def test_fingerprint_is_stable_when_only_line_position_shifts(self) -> None:
        root = self._fixture({"src/Feature.cpp": "#include <windows.h>\n"})
        before = boundary.scan_tree(root)
        ledger = boundary.build_ledger(before)
        root.joinpath("src/Feature.cpp").write_text("\n#include <windows.h>\n", encoding="utf-8")
        after = boundary.scan_tree(root)
        before_fingerprint = next(finding.fingerprint for finding in before if finding.rule == "native-include")
        after_fingerprint = next(finding.fingerprint for finding in after if finding.rule == "native-include")
        self.assertEqual(before_fingerprint, after_fingerprint)
        self.assertEqual([], boundary.compare_ledger(after, ledger))

    def test_identical_repeated_violations_remain_distinct(self) -> None:
        findings = self._scan({"src/Feature.cpp": "CloseHandle(handle);\nCloseHandle(handle);\n"})
        matches = [finding for finding in findings if finding.rule == "native-symbol"]
        self.assertEqual(2, len(matches))
        self.assertEqual(2, len({finding.fingerprint for finding in matches}))

    def test_ledger_rejects_a_new_unowned_finding(self) -> None:
        root = self._fixture({"src/Feature.cpp": "#include <windows.h>\n"})
        baseline = boundary.scan_tree(root)
        ledger = self._owned_ledger(baseline)
        root.joinpath("src/Other.cpp").write_text("std::filesystem::remove(path);\n", encoding="utf-8")
        errors = boundary.compare_ledger(boundary.scan_tree(root), ledger)
        self.assertTrue(any("unowned finding" in error.lower() for error in errors), errors)

    def test_ledger_rejects_a_stale_entry(self) -> None:
        root = self._fixture({"src/Feature.cpp": "#include <windows.h>\n"})
        baseline = boundary.scan_tree(root)
        ledger = self._owned_ledger(baseline)
        root.joinpath("src/Feature.cpp").unlink()
        errors = boundary.compare_ledger(boundary.scan_tree(root), ledger)
        self.assertTrue(any("stale ledger entry" in error.lower() for error in errors), errors)

    def test_ledger_rejects_an_invalid_owner_slice(self) -> None:
        findings = self._scan({"src/Feature.cpp": "#include <windows.h>\n"})
        ledger = self._owned_ledger(findings, owner_slice="S6-99")
        errors = boundary.compare_ledger(findings, ledger)
        self.assertTrue(any("invalid owner slice" in error.lower() for error in errors), errors)
        self.assertNotIn("S6-99", ALLOWED_OWNER_SLICES)

    def test_ledger_rejects_a_valid_but_wrong_owner_slice(self) -> None:
        findings = self._scan({"src/Database.cpp": "std::filesystem::remove(path);\n"})
        ledger = boundary.build_ledger(findings)
        ledger["findings"][0]["owner_slice"] = "S6-02"
        errors = boundary.compare_ledger(findings, ledger)
        self.assertTrue(any("owner mismatch" in error.lower() for error in errors), errors)

    def test_ledger_rejects_duplicates_and_non_migrate_dispositions(self) -> None:
        findings = self._scan({"src/Feature.cpp": "#include <windows.h>\n"})
        ledger = boundary.build_ledger(findings)
        ledger["findings"][0]["disposition"] = "ignore"
        ledger["findings"].append(dict(ledger["findings"][0]))
        errors = boundary.compare_ledger(findings, ledger)
        self.assertTrue(any("duplicate ledger fingerprint" in error.lower() for error in errors), errors)
        self.assertTrue(any("non-migrate disposition" in error.lower() for error in errors), errors)

    def test_ledger_rejects_wrong_schema_and_entry_tag(self) -> None:
        findings = self._scan({"src/Feature.cpp": "#include <windows.h>\n"})
        ledger = boundary.build_ledger(findings)
        ledger["schema"] = 2
        ledger["entry_tag"] = "main"
        errors = boundary.compare_ledger(findings, ledger)
        self.assertTrue(any("schema" in error.lower() for error in errors), errors)
        self.assertTrue(any("entry tag" in error.lower() for error in errors), errors)

    def test_ledger_assigns_known_path_owners(self) -> None:
        findings = self._scan({
            "src/App.cpp": "#include <windows.h>\n",
            "src/Database.cpp": "std::filesystem::remove(path);\nstruct ma_decoder* decoder;\n",
            "src/FileSystemWatcher.cpp": "auto handle = CreateFileW(path);\n",
            "src/CoverModule.cpp": "#include <windows.h>\n",
            "src/BpmDetector.cpp": "struct ma_decoder* decoder;\n",
            "src/AudioEngine.cpp": "struct ma_device* device;\n",
            "src/ModuleUI.cpp": "auto handle = CreateFileW(path);\n",
            "src/ProToolsBridge.cpp": "auto handle = CreateFileW(path);\n",
        })
        ledger = boundary.build_ledger(findings)
        owners = {(entry["path"], entry["rule"]): entry["owner_slice"] for entry in ledger["findings"]}
        self.assertEqual("S6-02", owners[("src/App.cpp", "native-include")])
        self.assertEqual("S6-03", owners[("src/Database.cpp", "filesystem-io")])
        self.assertEqual("S6-04", owners[("src/FileSystemWatcher.cpp", "native-symbol")])
        self.assertEqual("S6-05A", owners[("src/CoverModule.cpp", "native-include")])
        self.assertEqual("S6-05B", owners[("src/Database.cpp", "audio-io-type")])
        self.assertEqual("S6-05C", owners[("src/BpmDetector.cpp", "audio-io-type")])
        self.assertEqual("S6-06", owners[("src/AudioEngine.cpp", "audio-io-type")])
        self.assertEqual("S6-07A", owners[("src/ModuleUI.cpp", "native-symbol")])
        self.assertEqual("S6-07B", owners[("src/ProToolsBridge.cpp", "native-symbol")])

    def test_platform_adapter_accepts_only_reviewed_bootstrap_filesystem_findings(self) -> None:
        root = self._fixture({
            "src/bootstrap/WindowsBootstrapFilesystemWin32.cpp": (
                "#include <Windows.h>\n"
                "ScopedHandle volumePin(CreateFileW(\n"
            ),
            "docs/native-audit.md": "# Application-entry evidence\n",
            (
                "docs/plans/refactor/"
                "stage-6-bootstrap-filesystem-adapter-audit.md"
            ): "# Bootstrap filesystem adapter evidence\n",
        })
        findings = boundary.scan_tree(root)
        result = boundary.validate_audited_exceptions(
            findings,
            self._platform_adapter_exceptions(findings),
            root,
        )

        self.assertEqual(
            {"native-include", "native-symbol"},
            self._rules(findings),
        )
        self.assertEqual(
            frozenset(finding.fingerprint for finding in findings),
            result.audited_fingerprints,
        )
        self.assertEqual([], result.unresolved_findings)
        self.assertEqual([], result.errors)
        overlap = boundary.validate_no_overlap(
            boundary.build_ledger(findings),
            result.audited_fingerprints,
        )
        self.assertEqual(len(findings), len(overlap))

    def test_runtime_platform_adapter_accepts_only_reviewed_runtime_fingerprint(self) -> None:
        root = self._fixture({
            "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp": (
                "#include <windows.h>\n"
            ),
            "docs/native-audit.md": "# Application-entry evidence\n",
            (
                "docs/plans/refactor/"
                "stage-6-bootstrap-runtime-adapter-audit.md"
            ): "# Bootstrap runtime adapter evidence\n",
        })
        findings = boundary.scan_tree(root)
        result = boundary.validate_audited_exceptions(
            findings,
            self._runtime_platform_adapter_exceptions(findings),
            root,
        )

        self.assertEqual(1, len(findings))
        self.assertEqual(
            frozenset({findings[0].fingerprint}),
            result.audited_fingerprints,
        )
        self.assertEqual([], result.unresolved_findings)
        self.assertEqual([], result.errors)

    def test_bootstrap_native_vocabulary_covers_security_and_channel_calls(self) -> None:
        findings = self._scan({
            "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp": (
                "#include <knownfolders.h>\n"
                "#include <softpub.h>\n"
                "#include <wintrust.h>\n"
                "HANDLE job = CreateJobObjectW(nullptr, nullptr);\n"
                "DuplicateHandle(source, value, target, &copy, 0, FALSE, 0);\n"
                "if (!GetFileInformationByHandleEx(file, FileIdInfo, &info, sizeof(info))) return;\n"
                "if (!WinVerifyTrust(nullptr, &action, &trust)) return;\n"
            ),
            "src/bootstrap/WindowsBootstrapFilesystemWin32.cpp": (
                "#include <Windows.h>\n"
                "if (!ReadFile(file, bytes, size, &read, nullptr)) return;\n"
                "if (!FlushFileBuffers(file)) return;\n"
            ),
        })
        observed = {(finding.path, finding.rule, finding.token)
                    for finding in findings}
        self.assertTrue(
            {
                ("src/bootstrap/WindowsBootstrapRuntimeWin32.cpp", "native-include", "knownfolders.h"),
                ("src/bootstrap/WindowsBootstrapRuntimeWin32.cpp", "native-include", "softpub.h"),
                ("src/bootstrap/WindowsBootstrapRuntimeWin32.cpp", "native-include", "wintrust.h"),
                ("src/bootstrap/WindowsBootstrapRuntimeWin32.cpp", "native-symbol", "CreateJobObjectW"),
                ("src/bootstrap/WindowsBootstrapRuntimeWin32.cpp", "native-symbol", "DuplicateHandle"),
                ("src/bootstrap/WindowsBootstrapRuntimeWin32.cpp", "native-symbol", "GetFileInformationByHandleEx"),
                ("src/bootstrap/WindowsBootstrapRuntimeWin32.cpp", "native-symbol", "WinVerifyTrust"),
                ("src/bootstrap/WindowsBootstrapFilesystemWin32.cpp", "native-symbol", "ReadFile"),
                ("src/bootstrap/WindowsBootstrapFilesystemWin32.cpp", "native-symbol", "FlushFileBuffers"),
            }.issubset(observed),
            observed,
        )

    def test_runtime_platform_adapter_rejects_authority_substitution(self) -> None:
        runtime_evidence = (
            "docs/plans/refactor/"
            "stage-6-bootstrap-runtime-adapter-audit.md"
        )
        root = self._fixture({
            "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp": (
                "#include <windows.h>\n"
            ),
            "docs/native-audit.md": "# Application-entry evidence\n",
            runtime_evidence: "# Bootstrap runtime adapter evidence\n",
            (
                "docs/plans/refactor/"
                "stage-6-bootstrap-filesystem-adapter-audit.md"
            ): "# Bootstrap filesystem adapter evidence\n",
        })
        findings = boundary.scan_tree(root)
        cases = (
            (
                "disposition",
                "platform-entry",
                "unsupported platform-entry fingerprint",
            ),
            (
                "path",
                "src/bootstrap/OtherRuntimeWin32.cpp",
                "unsupported platform-adapter path",
            ),
            (
                "rule",
                "filesystem-io",
                "unsupported platform-adapter rule",
            ),
            (
                "token",
                "CreateFileW",
                "exception metadata mismatch",
            ),
            (
                "occurrence",
                2,
                "exception metadata mismatch",
            ),
            (
                "owner_slice",
                "BOOTSTRAP-FILESYSTEM",
                "expected BOOTSTRAP-RUNTIME",
            ),
            (
                "evidence_document",
                (
                    "docs/plans/refactor/"
                    "stage-6-bootstrap-filesystem-adapter-audit.md"
                ),
                "stage-6-bootstrap-runtime-adapter-audit.md",
            ),
        )
        for field, replacement, expected_error in cases:
            with self.subTest(field=field):
                register = self._runtime_platform_adapter_exceptions(findings)
                register["findings"][0][field] = replacement
                result = boundary.validate_audited_exceptions(
                    findings, register, root
                )
                self.assertTrue(
                    any(
                        expected_error.lower() in error.lower()
                        for error in result.errors
                    ),
                    result.errors,
                )
                self.assertEqual([findings[0]], result.unresolved_findings)

        register = self._runtime_platform_adapter_exceptions(findings)
        register["findings"][0]["fingerprint"] += "-substituted"
        result = boundary.validate_audited_exceptions(findings, register, root)
        self.assertTrue(
            any("stale exception entry" in error.lower()
                for error in result.errors),
            result.errors,
        )
        self.assertEqual([findings[0]], result.unresolved_findings)

    def test_runtime_platform_adapter_rejects_changed_source_fingerprint(self) -> None:
        root = self._fixture({
            "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp": (
                "#include <Windows.h>\n"
            ),
            "docs/native-audit.md": "# Application-entry evidence\n",
            (
                "docs/plans/refactor/"
                "stage-6-bootstrap-runtime-adapter-audit.md"
            ): "# Bootstrap runtime adapter evidence\n",
        })
        findings = boundary.scan_tree(root)
        result = boundary.validate_audited_exceptions(
            findings,
            self._runtime_platform_adapter_exceptions(findings),
            root,
        )

        self.assertEqual(1, len(findings))
        self.assertTrue(
            any(
                "unsupported platform-adapter fingerprint" in error.lower()
                for error in result.errors
            ),
            result.errors,
        )
        self.assertEqual([], list(result.audited_fingerprints))
        self.assertEqual(findings, result.unresolved_findings)

    def test_platform_adapter_rejects_unreviewed_path_rule_evidence_and_owner(self) -> None:
        root = self._fixture({
            "src/bootstrap/WindowsBootstrapFilesystemWin32.cpp": (
                "#include <Windows.h>\n"
                "ScopedHandle volumePin(CreateFileW(\n"
            ),
            "docs/native-audit.md": "# Application-entry evidence\n",
            (
                "docs/plans/refactor/"
                "stage-6-bootstrap-filesystem-adapter-audit.md"
            ): "# Bootstrap filesystem adapter evidence\n",
            "docs/wrong-adapter-audit.md": "# Wrong evidence\n",
        })
        findings = boundary.scan_tree(root)
        cases = (
            (
                "path",
                "src/bootstrap/OtherWin32.cpp",
                "unsupported platform-adapter path",
            ),
            ("rule", "filesystem-io", "unsupported platform-adapter rule"),
            (
                "evidence_document",
                "docs/wrong-adapter-audit.md",
                "platform-adapter evidence",
            ),
            ("owner_slice", "S6-02", "platform-adapter owner"),
        )
        for field, replacement, expected_error in cases:
            with self.subTest(field=field):
                register = self._platform_adapter_exceptions(findings)
                register["findings"][0][field] = replacement
                result = boundary.validate_audited_exceptions(
                    findings, register, root
                )
                self.assertTrue(
                    any(
                        expected_error in error.lower()
                        for error in result.errors
                    ),
                    result.errors,
                )
                self.assertEqual([findings[0]], result.unresolved_findings)

        disposition_register = self._platform_adapter_exceptions(findings)
        disposition_register["findings"][0]["owner_slice"] = "S6-02"
        disposition_register["findings"][0]["disposition"] = "platform-entry"
        disposition_result = boundary.validate_audited_exceptions(
            findings, disposition_register, root
        )
        self.assertTrue(
            any(
                "unsupported platform-entry fingerprint" in error.lower()
                for error in disposition_result.errors
            ),
            disposition_result.errors,
        )
        self.assertEqual(
            [findings[0]], disposition_result.unresolved_findings
        )

        other_root = self._fixture({
            "src/bootstrap/OtherWin32.cpp": "auto file = CreateFileW(path);\n",
            "docs/native-audit.md": "# Application-entry evidence\n",
            (
                "docs/plans/refactor/"
                "stage-6-bootstrap-filesystem-adapter-audit.md"
            ): "# Bootstrap filesystem adapter evidence\n",
        })
        other_findings = boundary.scan_tree(other_root)
        other_result = boundary.validate_audited_exceptions(
            other_findings,
            self._platform_adapter_exceptions(other_findings),
            other_root,
        )
        self.assertTrue(
            any(
                "unsupported platform-adapter path" in error.lower()
                for error in other_result.errors
            ),
            other_result.errors,
        )
        self.assertEqual(other_findings, other_result.unresolved_findings)

    def test_platform_adapter_rejects_unreviewed_fingerprint_on_approved_path(self) -> None:
        root = self._fixture({
            "src/bootstrap/WindowsBootstrapFilesystemWin32.cpp": (
                "#include <Windows.h>\n"
                "auto unexpected = CreateFileW(path, 0, 0, nullptr, 0, 0, nullptr);\n"
            ),
            "docs/native-audit.md": "# Application-entry evidence\n",
            (
                "docs/plans/refactor/"
                "stage-6-bootstrap-filesystem-adapter-audit.md"
            ): "# Bootstrap filesystem adapter evidence\n",
        })
        findings = boundary.scan_tree(root)
        result = boundary.validate_audited_exceptions(
            findings,
            self._platform_adapter_exceptions(findings),
            root,
        )

        self.assertEqual(2, len(findings))
        self.assertTrue(
            any(
                "unsupported platform-adapter fingerprint" in error.lower()
                for error in result.errors
            ),
            result.errors,
        )
        self.assertEqual(
            frozenset({findings[0].fingerprint}),
            result.audited_fingerprints,
        )
        self.assertEqual([findings[1]], result.unresolved_findings)

    def test_platform_adapter_evidence_rejects_resolved_escape(self) -> None:
        root = self._fixture({
            "src/bootstrap/WindowsBootstrapFilesystemWin32.cpp": (
                "#include <Windows.h>\n"
            ),
            "docs/native-audit.md": "# Application-entry evidence\n",
            (
                "docs/plans/refactor/"
                "stage-6-bootstrap-filesystem-adapter-audit.md"
            ): "# Bootstrap filesystem adapter evidence\n",
        })
        findings = boundary.scan_tree(root)
        register = self._platform_adapter_exceptions(findings)
        adapter_evidence = root / boundary.PLATFORM_ADAPTER_EVIDENCE
        outside_evidence = root.parent / f"{root.name}-outside-adapter.md"
        outside_evidence.write_text("# Outside evidence\n", encoding="utf-8")
        self.addCleanup(outside_evidence.unlink, missing_ok=True)
        path_type = type(root)
        real_resolve = path_type.resolve

        def resolve_with_escape(path: Path, *args, **kwargs) -> Path:
            if path == adapter_evidence:
                return outside_evidence
            return real_resolve(path, *args, **kwargs)

        with mock.patch.object(
            path_type,
            "resolve",
            autospec=True,
            side_effect=resolve_with_escape,
        ):
            result = boundary.validate_audited_exceptions(
                findings, register, root
            )

        self.assertTrue(
            any("escapes repository" in error.lower() for error in result.errors),
            result.errors,
        )
        self.assertEqual([], list(result.audited_fingerprints))
        self.assertEqual(findings, result.unresolved_findings)

    def test_platform_entry_remains_restricted_to_native_includes(self) -> None:
        root = self._fixture({
            "src/Feature.cpp": "auto file = CreateFileW(path);\n",
            "docs/native-audit.md": "# Evidence\n",
        })
        findings = boundary.scan_tree(root)
        result = boundary.validate_audited_exceptions(
            findings,
            self._audited_exceptions(findings),
            root,
        )

        self.assertEqual({"native-symbol"}, self._rules(findings))
        self.assertTrue(
            any(
                "unsupported exception rule" in error.lower()
                for error in result.errors
            ),
            result.errors,
        )
        self.assertEqual([], list(result.audited_fingerprints))
        self.assertEqual(findings, result.unresolved_findings)

    def test_platform_entry_rejects_unapproved_native_include_fingerprint(self) -> None:
        root = self._fixture({
            "src/bootstrap/WindowsBootstrapFilesystemWin32.cpp": (
                "#include <Windows.h>\n"
            ),
            "docs/native-audit.md": "# Evidence\n",
        })
        findings = boundary.scan_tree(root)
        result = boundary.validate_audited_exceptions(
            findings,
            self._audited_exceptions(findings),
            root,
        )

        self.assertEqual({"native-include"}, self._rules(findings))
        self.assertTrue(
            any(
                "unsupported platform-entry fingerprint" in error.lower()
                for error in result.errors
            ),
            result.errors,
        )
        self.assertEqual([], list(result.audited_fingerprints))
        self.assertEqual(findings, result.unresolved_findings)

    def test_audited_native_include_is_visible_but_not_unresolved(self) -> None:
        root = self._fixture({
            "src/main.cpp": "#include <windows.h>\n",
            "docs/native-audit.md": "# Evidence\n",
        })
        findings = boundary.scan_tree(root)
        result = boundary.validate_audited_exceptions(
            findings,
            self._audited_exceptions(findings),
            root,
        )

        self.assertEqual(1, len(findings))
        self.assertEqual(
            frozenset({findings[0].fingerprint}),
            result.audited_fingerprints,
        )
        self.assertEqual([], result.unresolved_findings)
        self.assertEqual([], result.errors)

    def test_audited_exceptions_reject_non_native_include(self) -> None:
        root = self._fixture({
            "src/Feature.cpp": "std::filesystem::remove(path);\n",
            "docs/native-audit.md": "# Evidence\n",
        })
        findings = boundary.scan_tree(root)
        result = boundary.validate_audited_exceptions(
            findings,
            self._audited_exceptions(findings),
            root,
        )

        self.assertTrue(any("unsupported exception rule" in error.lower() for error in result.errors), result.errors)
        self.assertEqual([], list(result.audited_fingerprints))
        self.assertEqual(findings, result.unresolved_findings)

    def test_audited_exceptions_reject_stale_and_duplicate_entries(self) -> None:
        root = self._fixture({
            "src/main.cpp": "#include <windows.h>\n",
            "docs/native-audit.md": "# Evidence\n",
        })
        findings = boundary.scan_tree(root)
        duplicate_register = self._audited_exceptions(findings)
        duplicate_register["findings"].append(dict(duplicate_register["findings"][0]))
        duplicate_result = boundary.validate_audited_exceptions(findings, duplicate_register, root)
        self.assertTrue(
            any("duplicate exception" in error.lower() for error in duplicate_result.errors),
            duplicate_result.errors,
        )

        stale_register = self._audited_exceptions(findings)
        root.joinpath("src/main.cpp").unlink()
        stale_result = boundary.validate_audited_exceptions(boundary.scan_tree(root), stale_register, root)
        self.assertTrue(
            any("stale exception" in error.lower() for error in stale_result.errors),
            stale_result.errors,
        )

    def test_audited_exceptions_reject_metadata_and_owner_mismatch(self) -> None:
        root = self._fixture({
            "src/main.cpp": "#include <windows.h>\n",
            "docs/native-audit.md": "# Evidence\n",
        })
        findings = boundary.scan_tree(root)
        register = self._audited_exceptions(findings)
        register["findings"][0]["token"] = "shellapi.h"
        register["findings"][0]["owner_slice"] = "S6-03"
        result = boundary.validate_audited_exceptions(findings, register, root)

        self.assertTrue(any("metadata mismatch" in error.lower() for error in result.errors), result.errors)
        self.assertTrue(any("owner mismatch" in error.lower() for error in result.errors), result.errors)
        self.assertEqual(findings, result.unresolved_findings)

    def test_audited_exceptions_require_existing_relative_evidence(self) -> None:
        root = self._fixture({
            "src/main.cpp": "#include <windows.h>\n",
            "docs/native-audit.md": "# Evidence\n",
        })
        findings = boundary.scan_tree(root)
        for evidence_document in ("docs/missing.md", str((root / "docs/native-audit.md").resolve())):
            with self.subTest(evidence_document=evidence_document):
                register = self._audited_exceptions(findings, evidence_document)
                result = boundary.validate_audited_exceptions(findings, register, root)
                self.assertTrue(
                    any("evidence document" in error.lower() for error in result.errors),
                    result.errors,
                )
                self.assertEqual(findings, result.unresolved_findings)

    def test_audited_exceptions_reject_ledger_overlap(self) -> None:
        root = self._fixture({
            "src/main.cpp": "#include <windows.h>\n",
            "docs/native-audit.md": "# Evidence\n",
        })
        findings = boundary.scan_tree(root)
        result = boundary.validate_audited_exceptions(
            findings,
            self._audited_exceptions(findings),
            root,
        )
        errors = boundary.validate_no_overlap(
            boundary.build_ledger(findings),
            result.audited_fingerprints,
        )
        self.assertTrue(any("overlap" in error.lower() for error in errors), errors)

    def test_require_empty_accepts_only_valid_audited_native_entries(self) -> None:
        root = self._fixture({
            "src/main.cpp": "#include <windows.h>\n",
            "docs/native-audit.md": "# Evidence\n",
        })
        findings = boundary.scan_tree(root)
        result = boundary.validate_audited_exceptions(
            findings,
            self._audited_exceptions(findings),
            root,
        )
        self.assertEqual(
            [],
            boundary.validate_empty(result.unresolved_findings, boundary.build_ledger([])),
        )

    def test_cli_audited_exception_closure_reports_all_counts(self) -> None:
        root = self._fixture({
            "src/main.cpp": "#include <windows.h>\n",
            "docs/native-audit.md": "# Evidence\n",
        })
        findings = boundary.scan_tree(root)
        ledger_path = root / "ledger.json"
        exceptions_path = root / "exceptions.json"
        ledger_path.write_text(json.dumps(boundary.build_ledger([])), encoding="utf-8")
        exceptions_path.write_text(
            json.dumps(self._audited_exceptions(findings)),
            encoding="utf-8",
        )
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = boundary.main([
                "--root", str(root),
                "--check-ledger", "ledger.json",
                "--check-exceptions", "exceptions.json",
                "--require-empty",
            ])

        self.assertEqual(0, result, stderr.getvalue())
        output = stdout.getvalue()
        self.assertIn("Scanned findings: 1", output)
        self.assertIn("Audited exceptions: 1", output)
        self.assertIn("Unresolved findings: 0", output)
        self.assertIn("Ledger findings: 0", output)

        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            missing_result = boundary.main([
                "--root", str(root),
                "--check-ledger", "ledger.json",
                "--check-exceptions", "missing.json",
                "--require-empty",
            ])
        self.assertEqual(1, missing_result)

    def test_repository_platform_adapters_are_exact_and_transport_is_resolved(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = boundary.main([
                "--root", str(ROOT),
                "--check-ledger", "docs/plans/refactor/stage-6-platform-migration-ledger.json",
                "--check-exceptions", "docs/plans/refactor/stage-6-platform-audited-exceptions.json",
                "--require-empty",
            ])

        self.assertEqual(0, result)
        output = stdout.getvalue()
        self.assertIn("Scanned findings: 593", output)
        self.assertIn("Audited exceptions: 593", output)
        self.assertIn("Unresolved findings: 0", output)
        self.assertIn("Ledger findings: 0", output)
        self.assertEqual("", stderr.getvalue())
        register = json.loads(
            (
                ROOT
                / "docs/plans/refactor/"
                "stage-6-platform-audited-exceptions.json"
            ).read_text(encoding="utf-8")
        )
        dispositions = [
            entry.get("disposition") for entry in register["findings"]
        ]
        self.assertEqual(13, dispositions.count("platform-entry"))
        self.assertEqual(580, dispositions.count("platform-adapter"))
        owners = [entry.get("owner_slice") for entry in register["findings"]]
        self.assertEqual(94, owners.count("BOOTSTRAP-FILESYSTEM"))
        self.assertEqual(480, owners.count("BOOTSTRAP-RUNTIME"))

        findings = boundary.scan_tree(ROOT)
        classification = boundary.validate_audited_exceptions(
            findings,
            register,
            ROOT,
        )
        self.assertEqual([], classification.errors)
        self.assertEqual(593, len(classification.audited_fingerprints))
        self.assertEqual(0, len(classification.unresolved_findings))

        runtime_fingerprints = {
            finding.fingerprint for finding in findings
            if finding.path == boundary.RUNTIME_PLATFORM_ADAPTER_PATH
        }
        registered_runtime_fingerprints = {
            entry["fingerprint"] for entry in register["findings"]
            if entry.get("owner_slice") == boundary.RUNTIME_PLATFORM_ADAPTER_OWNER
        }
        self.assertEqual(480, len(runtime_fingerprints))
        self.assertEqual(runtime_fingerprints, registered_runtime_fingerprints)

        self.assertEqual(
            runtime_fingerprints,
            set(boundary.APPROVED_RUNTIME_PLATFORM_ADAPTER_FINGERPRINTS),
        )

        filesystem_fingerprints = {
            finding.fingerprint for finding in findings
            if finding.path == boundary.PLATFORM_ADAPTER_PATH
        }
        registered_filesystem_fingerprints = {
            entry["fingerprint"] for entry in register["findings"]
            if entry.get("owner_slice") == boundary.PLATFORM_ADAPTER_OWNER
        }
        self.assertEqual(94, len(filesystem_fingerprints))
        self.assertEqual(
            filesystem_fingerprints,
            registered_filesystem_fingerprints,
        )
        self.assertEqual(
            filesystem_fingerprints,
            set(boundary.APPROVED_PLATFORM_ADAPTER_FINGERPRINTS),
        )
        self.assertEqual(
            set(boundary.APPROVED_PLATFORM_ENTRY_FINGERPRINTS),
            {
                entry["fingerprint"] for entry in register["findings"]
                if entry.get("disposition") == "platform-entry"
            },
        )

    def test_repository_windows_main_exception_metadata_matches_live_scan(self) -> None:
        register = json.loads(
            (
                ROOT
                / "docs/plans/refactor/"
                "stage-6-platform-audited-exceptions.json"
            ).read_text(encoding="utf-8")
        )
        findings = boundary.scan_tree(ROOT)
        classification = boundary.validate_audited_exceptions(
            findings,
            register,
            ROOT,
        )

        metadata_errors = [
            error
            for error in classification.errors
            if error.startswith("Exception metadata mismatch: src/WindowsMain.cpp|")
        ]
        self.assertEqual([], metadata_errors, classification.errors)

    def test_update_application_adapter_uses_live_renamed_path_without_retired_exceptions(self) -> None:
        self.assertEqual(
            "src/update/HarkenSearchUpdateComposition.cpp",
            boundary.UPDATE_APPLICATION_PLATFORM_ADAPTER_PATH,
        )
        self.assertTrue((ROOT / boundary.UPDATE_APPLICATION_PLATFORM_ADAPTER_PATH).is_file())
        self.assertEqual(frozenset(), boundary.APPROVED_UPDATE_APPLICATION_PLATFORM_ADAPTER_FINGERPRINTS)

    def test_stage6_report_matches_live_bootstrap_exception_counts(self) -> None:
        report = (ROOT / "docs/plans/refactor/stage-6-report.md").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "`593` exact audited entries",
            report,
            "the report must describe the current exact exception register",
        )
        self.assertIn("`94` `BOOTSTRAP-FILESYSTEM`", report)
        self.assertRegex(report, r"`480`\s+`BOOTSTRAP-RUNTIME`")
        self.assertNotIn("`193` exact audited entries", report)
        self.assertNotIn("`118`\n  `BOOTSTRAP-RUNTIME`", report)

    def test_validate_empty_rejects_findings_and_ledger(self) -> None:
        findings = self._scan({"src/Feature.cpp": "#include <windows.h>\n"})
        ledger = self._owned_ledger(findings)
        self.assertTrue(boundary.validate_empty(findings, {}))
        self.assertTrue(boundary.validate_empty([], ledger))

    def test_validate_empty_accepts_empty_inputs(self) -> None:
        self.assertEqual([], boundary.validate_empty([], {}))

    def test_database_destroy_failure_rechecks_main_path_through_pal(self) -> None:
        source = (ROOT / "src/Database.cpp").read_text(encoding="utf-8")
        body = source.split("bool Database::DestroyDatabasePackage", 1)[1].split(
            "// getCoverFacets is implemented above.", 1
        )[0]
        failure_check = body.split("if (!firstFailure.empty())", 1)[1]

        self.assertIn(
            "Platform::FileSystem::ProbePath(PathToUtf8(mainPath))",
            failure_check,
        )
        self.assertIn("Platform::FileSystem::PathStatus::Present", failure_check)
        self.assertNotIn("fs::exists(mainPath", failure_check)

    def test_database_destroy_retry_precheck_uses_pal_probe(self) -> None:
        source = (ROOT / "src/Database.cpp").read_text(encoding="utf-8")
        body = source.split("auto removePackageFile", 1)[1].split(
            "removePackageFile(mainDbPathUtf8)", 1
        )[0]

        self.assertIn("Platform::FileSystem::ProbePath(pathUtf8)", body)
        self.assertIn("Platform::FileSystem::PathStatus::Present", body)
        self.assertNotIn("fs::exists(p", body)

    def test_database_destroy_file_removal_retries_through_pal(self) -> None:
        source = (ROOT / "src/Database.cpp").read_text(encoding="utf-8")
        body = source.split("auto removePackageFile", 1)[1].split(
            "removePackageFile(mainDbPathUtf8)", 1
        )[0]

        self.assertIn("std::string error;", body)
        self.assertIn(
            "Platform::FileSystem::RemoveFile(pathUtf8, &error)", body
        )
        self.assertIn("error.empty()", body)
        self.assertIn("attempt < 3", body)
        self.assertIn("std::chrono::milliseconds(200)", body)
        self.assertIn('firstFailure = pathUtf8 + ": " + error;', body)
        self.assertNotIn("fs::remove", body)

    def test_database_destroy_sidecars_use_best_effort_pal_cleanup(self) -> None:
        source = (ROOT / "src/Database.cpp").read_text(encoding="utf-8")
        body = source.split("// Also remove WAL/SHM sidecars", 1)[1].split(
            "auto removePackageFile", 1
        )[0]

        probe = "Platform::FileSystem::ProbePath(PathToUtf8(sidecar))"
        remove = "Platform::FileSystem::RemoveFile(PathToUtf8(sidecar))"
        self.assertIn(probe, body)
        self.assertIn("Platform::FileSystem::PathStatus::Present", body)
        self.assertIn(remove, body)
        self.assertLess(body.index(probe), body.index(remove))
        self.assertNotIn("fs::exists(sidecar", body)
        self.assertNotIn("fs::remove(sidecar", body)
        self.assertNotIn("firstFailure", body)

    def test_database_destroy_package_directory_probes_use_pal(self) -> None:
        source = (ROOT / "src/Database.cpp").read_text(encoding="utf-8")
        body = source.split("// Package layout: also remove Cache subfolder", 1)[1].split(
            "if (!firstFailure.empty())", 1
        )[0]

        cache_probe = "Platform::FileSystem::ProbePath(PathToUtf8(cacheDir))"
        parent_probe = "Platform::FileSystem::ProbePath(PathToUtf8(parentDir))"
        self.assertIn(cache_probe, body)
        self.assertIn(parent_probe, body)
        self.assertEqual(2, body.count("Platform::FileSystem::PathStatus::Present"))
        self.assertLess(body.index(cache_probe), body.index("Platform::FileSystem::RemoveAll(PathToUtf8(cacheDir)"))
        self.assertIn("Platform::FileSystem::RemoveAll(PathToUtf8(cacheDir)", body)
        parent_remove = "Platform::FileSystem::RemoveFile(PathToUtf8(parentDir))"
        self.assertIn(parent_remove, body)
        self.assertLess(body.index(parent_probe), body.index(parent_remove))
        self.assertNotIn("fs::exists", body)
        self.assertNotIn("fs::remove(parentDir", body)
        self.assertNotIn("fs::remove_all(cacheDir", body)

    def test_export_worker_creates_generated_subfolder_through_pal(self) -> None:
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split("std::thread([&state, destDir, folderStrategy", 1)[1].split(
            "TransferPrefs transferPrefs", 1
        )[0]

        self.assertIn("Platform::FileSystem::CreateDirectories(subFolder);", body)
        self.assertNotIn("fs::create_directories", body)
        body = source.split("std::thread([&state, destDir, folderStrategy", 1)[1].split(
            "state.transferInProgress = false;", 1
        )[0]
        self.assertIn("Platform::FileSystem::CopyFile(", body)
        self.assertIn("record.filePath, destFile, resolved.wouldOverwrite", body)
        self.assertNotIn("fs::copy_file(fs::u8path(record.filePath)", body)

    def test_export_destination_directory_type_uses_pal(self) -> None:
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split('cmd == "start_export"', 1)[1].split(
            "SaveAppSettings(state);", 1
        )[0]

        self.assertIn("Platform::FileSystem::IsDirectory(destDir)", body)
        self.assertNotIn("fs::is_directory", body)
        self.assertIn("destination is not a folder", body)

    def test_export_destination_presence_uses_pal_tristate_probe(self) -> None:
        source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        body = source.split('cmd == "start_export"', 1)[1].split(
            "SaveAppSettings(state);", 1
        )[0]

        probe = "Platform::FileSystem::ProbePath(destDir, &destinationProbeError)"
        self.assertIn("std::string destinationProbeError;", body)
        self.assertIn(probe, body)
        self.assertIn("Platform::FileSystem::PathStatus::Missing", body)
        self.assertIn("Platform::FileSystem::PathStatus::Error", body)
        self.assertIn("? destinationProbeError", body)
        # H1c: pin follows the E1A keyed-form rewrite (setTransferStatus carries the
        # rendered Literal on this main-thread QA path; the raw field is render-only now).
        self.assertIn('snprintf(wtb1, sizeof(wtb1), L(TR_MkdirFailFmt), destinationError.c_str())', body)
        self.assertIn('state.setTransferStatus(wtb1, AppState::TransferStatusKind::Error);', body)
        self.assertLess(body.index(probe), body.index("Platform::FileSystem::CreateDirectories(destDir)"))
        self.assertIn("Platform::FileSystem::CreateDirectories(destDir)", body)
        self.assertNotIn("fs::exists", body)
        self.assertNotIn("fs::create_directories", body)

    def test_main_export_destination_presence_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("if (ImGui::Button(L(Export_Start)", 1)[1].split(
            "std::thread([&state, destDir, folderStrat, existBehav, keepSource]()",
            1,
        )[0]

        probe = "Platform::FileSystem::ProbePath(destDir, &destinationProbeError)"
        self.assertIn("std::string destinationProbeError;", body)
        self.assertIn(probe, body)
        self.assertIn("Platform::FileSystem::IsDirectory(destDir)", body)
        self.assertIn("Platform::FileSystem::PathStatus::Missing", body)
        self.assertIn("Platform::FileSystem::PathStatus::Error", body)
        self.assertLess(body.index(probe), body.index("Platform::FileSystem::CreateDirectories(destDir)"))
        self.assertIn("Platform::FileSystem::CreateDirectories(destDir)", body)
        self.assertNotIn("fs::exists", body)
        self.assertNotIn("fs::is_directory", body)
        self.assertNotIn("fs::create_directories", body)

    def test_main_new_database_overwrite_preserves_package_through_filesystem_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("// 新建数据库弹窗", 1)[1].split("newDbDialogSurfaceStyle.reset();", 1)[0]
        replacement = (ROOT / "src/DatabasePackageReplacement.cpp").read_text(encoding="utf-8")

        self.assertIn("bool RemoveAll(", header)
        self.assertNotIn("RemoveAll", body)
        self.assertIn("newDbCacheMode, confirmOverwrite, deferredStart)", body)
        self.assertIn("state.infoEditSession.awaitingChoice", body)
        self.assertIn("files::Rename(root_, backup, &error_)", replacement)
        self.assertIn("files::RemoveAll(backup_, &error_)", replacement)
        self.assertIn("files::Rename(backup_, root_, &cleanupError)", replacement)
        self.assertNotIn("fs::remove_all", replacement)

    def test_main_settings_export_source_probe_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("if (ImGui::Button(L(Set_ExportSettings)", 1)[1].split(
            "if (HoveredForHelp()) Aura::SetTooltip",
            1,
        )[0]

        self.assertIn(
            "Platform::FileSystem::ProbePath(srcPath) == Platform::FileSystem::PathStatus::Present",
            body,
        )
        self.assertNotIn("fs::exists", body)

    def test_main_settings_import_export_copy_uses_filesystem_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        export_body = source.split("if (ImGui::Button(L(Set_ExportSettings)", 1)[1].split(
            "if (HoveredForHelp()) Aura::SetTooltip",
            1,
        )[0]
        import_body = source.split("if (ImGui::Button(L(Set_ImportSettings)", 1)[1].split(
            "if (HoveredForHelp()) Aura::SetTooltip",
            1,
        )[0]

        self.assertIn("bool CopyFile(", header)
        self.assertIn(
            "Platform::FileSystem::CopyFile(srcPath, exportPath, true)",
            export_body,
        )
        self.assertIn(
            "Platform::FileSystem::CopyFile(importPath, dstPath, true)",
            import_body,
        )
        self.assertLess(
            import_body.index(
                "Platform::FileSystem::CopyFile(importPath, dstPath, true)"
            ),
            import_body.index("LoadAppSettings(state)"),
        )
        self.assertNotIn("fs::copy_file", export_body)
        self.assertNotIn("fs::copy_file", import_body)

    def test_main_database_package_root_creation_uses_filesystem_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        body = (ROOT / "src/DatabasePackageReplacement.cpp").read_text(encoding="utf-8")

        self.assertIn("bool CreateDirectories(", header)
        self.assertIn("files::ProbePath(root_, &error_)", body)
        self.assertIn("files::CreateDirectories(root_)", body)
        self.assertNotIn("fs::create_directories", body)

    def test_channel_export_workflow_folder_creation_uses_filesystem_pal(self) -> None:
        header = (ROOT / "src" / "platform" / "filesystem.h").read_text(encoding="utf-8")
        body = (ROOT / "src" / "SingleChannelExportWorkflow.cpp").read_text(encoding="utf-8")

        self.assertIn("bool EnsureDirectoryExists(", header)
        self.assertIn("Platform::FileSystem::EnsureDirectoryExists(", body)
        self.assertIn("Platform::FileSystem::IsDirectory(request.destinationDirectory)", body)
        self.assertNotIn("std::filesystem::create_directories", body)
        self.assertNotIn("std::filesystem::is_directory", body)

    def test_main_transfer_subfolder_creation_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("if (ImGui::Button(L(Export_Start), ImVec2(120, 0)) && !state.transferInProgress)", 1)[1].split(
            "TransferDestinationResolution resolved =",
            1,
        )[0]

        self.assertIn("Platform::FileSystem::CreateDirectories(subFolder)", body)
        self.assertNotIn("fs::create_directories(fs::u8path(subFolder), ec2)", body)

    def test_main_transfer_copy_uses_filesystem_pal(self) -> None:
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        body = source.split("std::thread([&state, destDir, folderStrat, existBehav, keepSource]()", 1)[1].split(
            "if (ImGui::Button(L(Set_ExportSettings),", 1
        )[0]

        self.assertIn("Platform::FileSystem::CopyFile(", body)
        self.assertIn("rec.filePath, destFile, resolved.wouldOverwrite", body)
        self.assertNotIn("fs::copy_file(fs::u8path(rec.filePath), fs::u8path(destFile)", body)

    def test_comments_and_strings_are_not_scanned_as_platform_usage(self) -> None:
        findings = self._scan({"src/Feature.cpp": "// #include <windows.h>\n/* CreateFileW and std::filesystem::remove(path) */\nconst char* text = \"#include <unistd.h> open CreateFileW\";\n"})
        self.assertEqual([], findings)


if __name__ == "__main__":
    unittest.main()
