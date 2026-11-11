import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") + \
    (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
HEADER = (ROOT / "src" / "platform" / "durable_file_ops.h").read_text(encoding="utf-8")
WIN32 = (ROOT / "src" / "platform" / "durable_file_ops_win32.cpp").read_text(encoding="utf-8")
MACOS = (ROOT / "src" / "platform" / "durable_file_ops_macos.cpp").read_text(encoding="utf-8")
DOCTOR = (ROOT / "src" / "ReaperBridgeDoctor.cpp").read_text(encoding="utf-8")


class ReaperBridgeDoctorDurableFileOpsCopyContract(unittest.TestCase):
    def test_api_and_real_fixture_target_are_registered(self):
        self.assertIn("DurableFileOpResult CopyFileExclusiveAndFlush(", HEADER)
        self.assertIn("sourcePathUtf8", HEADER)
        self.assertIn("targetPathUtf8", HEADER)
        self.assertIn("TestPlatformDurableFileOpsCopy", CMAKE)
        self.assertIn("TestReaperBridgeDoctorDurableFileOpsCopyContract", CMAKE)

    def test_windows_copy_is_exclusive_and_captures_copy_error_before_flush(self):
        for token in (
            "CopyFileW",
            "TRUE",
            "TargetExists",
            "GetLastError",
            "FlushExistingFileToDisk",
            "CopyFailed",
        ):
            self.assertIn(token, WIN32)

    def test_macos_copy_has_a_native_runtime_fixture(self):
        self.assertIn("DurableFileOpResult CopyFileExclusiveAndFlush", MACOS)
        self.assertIn("TestMacDurableSettings", CMAKE)
        self.assertIn("tests/test_macos_durable_settings.cpp", CMAKE)
        self.assertNotIn("CopyFileW", MACOS)

    def test_doctor_uses_copy_pal_for_stage_and_backup_only(self):
        self.assertIn("platform/durable_file_ops.h", DOCTOR)
        self.assertIn("Platform::FileSystem::CopyFileExclusiveAndFlush", DOCTOR)
        self.assertNotIn("CopyFileW", DOCTOR)


if __name__ == "__main__":
    unittest.main()
