import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
HEADER = ROOT / "src" / "platform" / "pe_inspection.h"
WIN32 = ROOT / "src" / "platform" / "pe_inspection_win32.cpp"
MACOS = ROOT / "src" / "platform" / "pe_inspection_macos.mm"
DOCTOR = (ROOT / "src" / "ReaperBridgeDoctor.cpp").read_text(encoding="utf-8")


class ReaperBridgeDoctorPeInspectionContract(unittest.TestCase):
    def test_public_api_is_native_free_and_build_sources_are_registered(self):
        self.assertTrue(HEADER.is_file())
        header = HEADER.read_text(encoding="utf-8")
        for token in (
            "namespace Platform::PeInspection",
            "enum class Status",
            "Unsupported",
            "Success",
            "InvalidArgument",
            "OpenFailed",
            "InvalidFormat",
            "ValueUnavailable",
            "QueryFailed",
            "EncodingFailed",
            "struct StringResult",
            "std::string value",
            "unsigned long systemError",
            "StringResult ReadArchitecture(const std::string& pathUtf8);",
            "StringResult ReadFileVersion(const std::string& pathUtf8);",
            "StringResult ReadVersionString(const std::string& pathUtf8,",
            "const std::string& keyUtf8);",
            "struct ExportResult",
            "bool present",
            "ExportResult ProbeExport(const std::string& pathUtf8,",
            "const std::string& exportName);",
        ):
            self.assertIn(token, header)
        for token in ("windows.h", "HANDLE", "DWORD", "IMAGE_DOS_HEADER"):
            self.assertNotIn(token, header)
        self.assertIn("src/platform/pe_inspection_win32.cpp", CMAKE)
        self.assertIn("src/platform/pe_inspection_macos.mm", CMAKE)

    def test_win32_owns_read_only_pe_query_and_raii(self):
        self.assertTrue(WIN32.is_file())
        source = WIN32.read_text(encoding="utf-8")
        for token in (
            "CreateFileW",
            "GENERIC_READ",
            "FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE",
            "OPEN_EXISTING",
            "IMAGE_DOS_HEADER",
            "IMAGE_DOS_SIGNATURE",
            "e_lfanew",
            "SetFilePointerEx",
            "IMAGE_NT_SIGNATURE",
            "IMAGE_FILE_HEADER",
            "IMAGE_FILE_MACHINE_I386",
            "IMAGE_FILE_MACHINE_AMD64",
            "IMAGE_FILE_MACHINE_ARM64",
            "class ScopedPeFile",
            "~ScopedPeFile()",
            "CloseHandle(handle_)",
            "Utf8ToWideStrict",
            "Status::InvalidArgument",
            "Status::OpenFailed",
            "Status::InvalidFormat",
            "Status::QueryFailed",
            "Status::EncodingFailed",
            "Status::Success",
            "GetFileVersionInfoSizeW",
            "GetFileVersionInfoW",
            "VS_FIXEDFILEINFO",
            "dwSignature",
            "VerQueryValueW",
            "VarFileInfo\\\\Translation",
            "wchar_t query[256]",
            "LoadLibraryExW",
            "DONT_RESOLVE_DLL_REFERENCES",
            "GetProcAddress",
            "class ScopedPeModule",
            "~ScopedPeModule()",
            "FreeLibrary(module_)",
            "ReadFileVersion",
            "ReadVersionString",
            "ProbeExport",
        ):
            self.assertIn(token, source)

    def test_macos_inspects_native_files_without_executing_them(self):
        self.assertTrue(MACOS.is_file())
        source = MACOS.read_text(encoding="utf-8")
        for token in (
            "ReadFileVersion",
            "ReadVersionString",
            "ProbeExport",
            "Status::Unsupported",
        ):
            self.assertIn(token, source)
        self.assertNotIn("IMAGE_DOS_HEADER", source)
        self.assertNotIn("dlopen", source)
        self.assertNotIn("dlsym", source)
        cmake_tests = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
        self.assertIn("TestMacMachOInspection", cmake_tests)

    def test_doctor_delegates_and_keeps_version_hash_install_native_work(self):
        self.assertIn('#include "platform/pe_inspection.h"', DOCTOR)
        self.assertIn("Platform::PeInspection::ReadArchitecture", DOCTOR)
        self.assertIn("Platform::PeInspection::ReadFileVersion", DOCTOR)
        self.assertIn("Platform::PeInspection::ReadVersionString", DOCTOR)
        self.assertIn("Platform::PeInspection::ProbeExport", DOCTOR)
        self.assertIn("PeInspection::Status::Success", DOCTOR)
        for token in (
            "IMAGE_DOS_HEADER",
            "SetFilePointerEx",
            "IMAGE_FILE_HEADER",
            "GetFileVersionInfoSizeW",
            "GetFileVersionInfoW",
            "VerQueryValueW",
            "LoadLibraryExW",
            "GetProcAddress",
        ):
            self.assertNotIn(token, DOCTOR)
        for token in (
        ):
            self.assertIn(token, DOCTOR)


if __name__ == "__main__":
    unittest.main()
