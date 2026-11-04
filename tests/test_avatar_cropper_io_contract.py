import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "AvatarCropperIO.cpp").read_text(encoding="utf-8")
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
MAIN += (ROOT / "src" / "SettingsAvatarTools.inl").read_text(encoding="utf-8")
CMAKE = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(encoding="utf-8")


class AvatarCropperIoContractTests(unittest.TestCase):
    def test_load_uses_filesystem_bytes_and_stb_memory_decoder(self):
        self.assertIn('#include "platform/filesystem.h"', SOURCE)
        self.assertIn("Platform::FileSystem::ReadFileBytes", SOURCE)
        self.assertIn("std::numeric_limits<int>::max()", SOURCE)
        self.assertIn("stbi_load_from_memory", SOURCE)
        self.assertNotIn("stbi_load(pathUtf8.c_str()", SOURCE)

    def test_save_uses_stream_writer_and_stb_callback(self):
        self.assertIn("Platform::FileSystem::StreamWriter", SOURCE)
        self.assertIn("writer.Open(destPathUtf8, false, true)", SOURCE)
        self.assertIn("stbi_write_png_to_func", SOURCE)
        self.assertIn("writer.Write(std::string_view", SOURCE)
        self.assertIn("writer.Flush()", SOURCE)
        self.assertIn("writer.Close()", SOURCE)
        self.assertIn("encoded && writeOk && flushed", SOURCE)

    def test_native_and_stdio_file_ownership_is_retired(self):
        for token in (
            "windows.h",
            "AvatarIO_Utf8ToWide",
            "MultiByteToWideChar",
            "_wfopen_s",
            "FILE*",
            "fwrite",
            "fclose",
        ):
            self.assertNotIn(token, SOURCE)

    def test_crop_and_main_call_sites_remain_owned_by_existing_modules(self):
        self.assertIn("ComputeSourceRect(crop)", SOURCE)
        self.assertIn("stbir_resize_uint8_linear", SOURCE)
        self.assertIn("LoadImageForCrop(selected", MAIN)
        self.assertIn("SaveCroppedAvatarWithSettings(s_cropPixels", MAIN)

    def test_focused_runtime_target_is_registered(self):
        self.assertIn("add_executable(TestAvatarCropperIO", CMAKE)
        self.assertIn("tests/test_avatar_cropper_io.cpp", CMAKE)
        self.assertIn("src/AvatarCropperIO.cpp", CMAKE)
        self.assertIn("src/platform/filesystem_std.cpp", CMAKE)


if __name__ == "__main__":
    unittest.main()
