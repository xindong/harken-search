"""Contract test for main.cpp's shared UTF-8 PAL call sites."""

from pathlib import Path
import os
import re


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start + len(signature))
    return source[start:end]


def main() -> int:
    source_path = Path(os.environ.get("ARIS_MAIN_CPP", "src/main.cpp"))
    source = source_path.read_text(encoding="utf-8")

    wav = function_body(
        source,
        "static int GetWavBitDepthUtf8",
        "// Custom ImGui Pitch Wheel Widget",
    )
    naming = function_body(
        source,
        "static std::wstring Utf8ToWideForNaming",
        "// Validate file naming convention",
    )
    load_settings = function_body(
        source,
        "static std::unordered_map<std::string, std::string> LoadKeyValueFile",
        "static bool SaveAppSettingsKeyValueFile",
    )
    save_settings = function_body(
        source,
        "static bool SaveAppSettingsKeyValueFile",
        "static fs::path Utf8ToPathMain",
    )
    settings_repair = function_body(
        source,
        "static std::string RepairUtf8AsAnsiMojibakeForSettings",
        "static fs::path Utf8ToPathMain",
    )
    cover_import = function_body(
        source,
        "// COVER IMPORT DIALOG",
        "// Cover remove confirmation",
    )
    cover_dialog_backend = Path(
        "src/platform/cover_image_import_dialog_win32.cpp"
    ).read_text(encoding="utf-8")
    region_export = function_body(
        source,
        "//  MODULE 4: Loop + \u5bfc\u51fa\u9009\u533a",
        "const std::string transportItemCountText",
    )
    region_dialog_backend = Path(
        "src/platform/region_wav_export_dialog_win32.cpp"
    ).read_text(encoding="utf-8")
    folder_dialog = function_body(
        source,
        "std::string OpenFolderDialog(HWND hwnd)",
        "// Application State",
    )

    assert "Platform::FileSystem::StreamReader file" in wav
    assert "Utf8ToPathMain(filePathUtf8)" not in wav
    assert "MultiByteToWideChar" not in wav
    assert "Utf8ToWide(str)" in naming
    assert "MultiByteToWideChar" not in naming
    assert not re.search(r"wstring\s+wFilePath\(wideLen\s*-\s*1", wav)
    assert "Platform::FileSystem::ReadFileText(pathUtf8, contents)" in load_settings
    assert "Utf8ToPathMain(pathUtf8)" not in load_settings
    assert "MultiByteToWideChar" not in load_settings
    assert "harken::settings::SaveKeyValueFile(pathUtf8,items)" in save_settings
    save_settings += Path("src/SettingsFileWriter.h").read_text(encoding="utf-8")
    assert "Platform::FileSystem::StreamWriter" in save_settings
    assert "Utf8ToPathMain(pathUtf8)" not in save_settings
    assert "MultiByteToWideChar" not in save_settings
    assert (
        "Platform::TextEncoding::RepairUtf8MojibakeFromCp936(repaired)"
        in settings_repair
    )
    assert "WideCharToMultiByte" not in settings_repair
    assert "MultiByteToWideChar" not in settings_repair
    assert (
        "Platform::CoverDialogs::OpenCoverImageImportFileDialogUtf8(g_hwnd)"
        in cover_import
    )
    assert "GetOpenFileNameW" not in cover_import
    assert "WideToUtf8(std::wstring(szFile))" in cover_dialog_backend
    assert "WideCharToMultiByte" not in cover_import
    assert (
        "Platform::RegionDialogs::SaveRegionWavFileDialogUtf8(g_hwnd)"
        in region_export
    )
    assert "GetSaveFileNameW" not in region_export
    assert (
        "TextEncoding::WideToUtf8(std::wstring(szFileName))"
        in region_dialog_backend
    )
    assert "WideCharToMultiByte" not in region_export
    assert (
        'return Platform::BrowseFolderDialogModern(hwnd, u8"选择音频文件夹");'
        in folder_dialog
    )
    assert "WideCharToMultiByte" not in folder_dialog
    assert "Platform::FreeComTaskMemory(pszPath)" not in folder_dialog
    print("PASS: main.cpp encoding call sites use PAL helpers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
