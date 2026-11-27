import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
HEADER_PATH = ROOT / "src" / "platform" / "durable_file_ops.h"
HEADER = HEADER_PATH.read_text(encoding="utf-8") if HEADER_PATH.is_file() else ""
WIN32_PATH = ROOT / "src" / "platform" / "durable_file_ops_win32.cpp"
WIN32 = WIN32_PATH.read_text(encoding="utf-8") if WIN32_PATH.is_file() else ""
MACOS_PATH = ROOT / "src" / "platform" / "durable_file_ops_macos.cpp"
MACOS = MACOS_PATH.read_text(encoding="utf-8") if MACOS_PATH.is_file() else ""
DOCTOR = (ROOT / "src" / "ReaperBridgeDoctor.cpp").read_text(encoding="utf-8")


class ReaperBridgeDoctorDurableFileOpsContract(unittest.TestCase):
    def test_native_free_api_and_ctest_registration(self):
        for token in (
            "namespace Platform::FileSystem",
            "enum class DurableFileOpStatus",
            "InvalidArgument",
            "EncodingFailed",
            "OpenFailed",
            "FlushFailed",
            "struct DurableFileOpResult",
            "uint32_t systemError",
            "DurableFileOpResult FlushExistingFileToDisk(const std::string& pathUtf8);",
        ):
            self.assertIn(token, HEADER)
        for token in ("windows.h", "HANDLE", "FlushFileBuffers"):
            self.assertNotIn(token, HEADER)
        self.assertIn("TestReaperBridgeDoctorDurableFileOpsContract", CMAKE)

    def test_windows_owns_flush_lifecycle_and_primary_error(self):
        for token in (
            "GENERIC_WRITE",
            "FILE_SHARE_READ | FILE_SHARE_DELETE",
            "OPEN_EXISTING",
            "FlushFileBuffers",
            "CloseHandle",
            "FlushFailed",
        ):
            self.assertIn(token, WIN32)
        self.assertIn("systemError", WIN32)

    def test_macos_implements_durable_flush_and_keeps_real_errors(self):
        for token in ("F_FULLFSYNC", "FlushDescriptor", "FlushFailed", "OpenFailed"):
            self.assertIn(token, MACOS)
        # Plain fsync may only stand in for a filesystem that rejects the
        # command; a real write error such as EIO must surface.
        self.assertIn("FullsyncUnsupported", MACOS)
        self.assertNotIn("DurableFileOpStatus::Unsupported", MACOS)
        self.assertNotIn("FlushFileBuffers", MACOS)

    def test_doctor_delegates_and_preserves_other_write_operations(self):
        self.assertIn("platform/durable_file_ops.h", DOCTOR)
        for token in ("CreateFileW", "FlushFileBuffers", "CloseHandle"):
            self.assertNotIn(token, DOCTOR)


if __name__ == "__main__":
    unittest.main()
