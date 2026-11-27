import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") + \
    (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
HEADER = (ROOT / "src" / "platform" / "durable_file_ops.h").read_text(encoding="utf-8")
WIN32 = (ROOT / "src" / "platform" / "durable_file_ops_win32.cpp").read_text(encoding="utf-8")
MACOS = (ROOT / "src" / "platform" / "durable_file_ops_macos.cpp").read_text(encoding="utf-8")
DOCTOR = (ROOT / "src" / "ReaperBridgeDoctor.cpp").read_text(encoding="utf-8")


class ReaperBridgeDoctorDurableFileOpsReplaceDeleteContract(unittest.TestCase):
    def test_api_and_fixture_are_registered(self):
        self.assertIn("DurableFileOpResult ReplaceFileWriteThrough(", HEADER)
        self.assertIn("DurableFileOpResult DeleteFileIfPresent(", HEADER)
        self.assertIn("TestPlatformDurableFileOpsReplaceDelete", CMAKE)
        self.assertIn("TestReaperBridgeDoctorDurableFileOpsReplaceDeleteContract", CMAKE)

    def test_windows_replace_is_single_write_through_move_without_fallback(self):
        for token in (
            "MoveFileExW",
            "MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH",
            "ReplaceFailed",
            "GetLastError",
        ):
            self.assertIn(token, WIN32)
        replace_body = WIN32.split("ReplaceFileWriteThrough", 1)[-1].split(
            "DeleteFileIfPresent", 1
        )[0]
        self.assertNotIn("DeleteFileW", replace_body)
        self.assertNotIn("CopyFileW", replace_body)

    def test_windows_delete_maps_only_missing_file_to_success(self):
        for token in ("DeleteFileW", "ERROR_FILE_NOT_FOUND", "ERROR_PATH_NOT_FOUND", "DeleteFailed"):
            self.assertIn(token, WIN32)

    def test_macos_replaces_and_deletes_with_directory_sync(self):
        for token in (
            "renamex_np",
            "RENAME_EXCL",
            "DurableFileOpStatus::ReplaceFailed",
            "DurableFileOpStatus::DeleteFailed",
            "FlushDirectoryOf",
        ):
            self.assertIn(token, MACOS)
        self.assertNotIn("DurableFileOpStatus::Unsupported", MACOS)
        self.assertNotIn("MoveFileExW", MACOS)
        self.assertNotIn("DeleteFileW", MACOS)

    def test_doctor_delegates_install_file_ops_and_keeps_other_native_work(self):
        self.assertIn("platform/durable_file_ops.h", DOCTOR)
        self.assertIn("Platform::FileSystem::ReplaceFileWriteThrough", DOCTOR)
        self.assertIn("Platform::FileSystem::DeleteFileIfPresent", DOCTOR)
        for token in ("DeleteFileW", "MoveFileExW", "GetLastError"):
            self.assertNotIn(token, DOCTOR)


if __name__ == "__main__":
    unittest.main()
