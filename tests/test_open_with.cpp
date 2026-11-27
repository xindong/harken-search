// tests/test_open_with.cpp
// Unit tests for F-034 Open With / Default Player integration.
// Tests: IsAudioFileSupported extension check, path encoding round-trip,
//        registry write/read/delete round-trip.
#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "Utils.h"   // IsAudioFileSupported, WideToUtf8, Utf8ToWide

#ifdef _WIN32
#if __has_include("platform/explorer_shell_path.h")
#include "platform/explorer_shell_path.h"
#define AURA_HAS_EXPLORER_SHELL_PATH 1
#else
#define AURA_HAS_EXPLORER_SHELL_PATH 0
#endif
#endif

// ---- Test helpers ----
static int g_pass = 0, g_fail = 0;
#define EXPECT_TRUE(expr) \
    do { if (expr) { printf("PASS: " #expr "\n"); ++g_pass; } \
         else      { printf("FAIL: " #expr "\n"); ++g_fail; } } while(0)
#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))
#define EXPECT_EQ(a,b) EXPECT_TRUE((a)==(b))

#ifdef _WIN32
static void expectNamed(bool cond, const char* name) {
    if (cond) {
        printf("PASS: %s\n", name);
        ++g_pass;
    } else {
        printf("FAIL: %s\n", name);
        ++g_fail;
    }
}

static std::string readRepoTextFile(const std::filesystem::path& relativePath) {
    const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    std::ifstream in(repoRoot / relativePath, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
#endif

// ---- Test 1: IsAudioFileSupported extension check ----
static void test_audio_extensions() {
    // Supported formats
    EXPECT_TRUE(IsAudioFileSupported("song.wav"));
    EXPECT_TRUE(IsAudioFileSupported("track.mp3"));
    EXPECT_TRUE(IsAudioFileSupported("audio.flac"));
    EXPECT_TRUE(IsAudioFileSupported("file.ogg"));
    EXPECT_TRUE(IsAudioFileSupported("clip.m4a"));
    EXPECT_TRUE(IsAudioFileSupported("sound.aac"));
    EXPECT_TRUE(IsAudioFileSupported("music.wma"));
    EXPECT_TRUE(IsAudioFileSupported("sample.opus"));
    // Case insensitive
    EXPECT_TRUE(IsAudioFileSupported("SONG.WAV"));
    EXPECT_TRUE(IsAudioFileSupported("Track.MP3"));
    // Non-audio formats
    EXPECT_FALSE(IsAudioFileSupported("document.pdf"));
    EXPECT_FALSE(IsAudioFileSupported("image.png"));
    EXPECT_FALSE(IsAudioFileSupported("video.mp4"));
    EXPECT_FALSE(IsAudioFileSupported("archive.zip"));
    EXPECT_FALSE(IsAudioFileSupported(""));
}

// ---- Test 2: UTF-8 / wide path round-trip (covers CJK paths) ----
static void test_path_encoding() {
    // ASCII path
    {
        std::string utf8 = "C:/Fixture/audio.wav";
        std::wstring wide = Utf8ToWide(utf8);
        std::string back = WideToUtf8(wide);
        EXPECT_EQ(utf8, back);
    }
    // Path with spaces
    {
        std::string utf8 = "C:/My Music/Some Track.mp3";
        std::wstring wide = Utf8ToWide(utf8);
        std::string back = WideToUtf8(wide);
        EXPECT_EQ(utf8, back);
    }
    // CJK path (Chinese characters)
    {
        std::string utf8 = u8"C:/\u97f3\u9891\u6587\u4ef6/track.wav";
        std::wstring wide = Utf8ToWide(utf8);
        std::string back = WideToUtf8(wide);
        EXPECT_EQ(utf8, back);
    }
}

// ---- Test 3: Registry write / read / delete round-trip ----
#ifdef _WIN32
static void test_registry_roundtrip() {
    const wchar_t* testKey = L"Software\\HarkenSearchTest_F034";
    const wchar_t* testVal = L"TestProgId";
    const wchar_t* testData = L"Auro.AudioFile";

    // Write
    HKEY hKey = nullptr;
    LONG res = RegCreateKeyExW(HKEY_CURRENT_USER, testKey, 0, nullptr, 0,
                               KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &hKey, nullptr);
    EXPECT_EQ(res, (LONG)ERROR_SUCCESS);
    if (res == ERROR_SUCCESS) {
        res = RegSetValueExW(hKey, testVal, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(testData),
            (DWORD)((wcslen(testData) + 1) * sizeof(wchar_t)));
        EXPECT_EQ(res, (LONG)ERROR_SUCCESS);

        // Read back
        wchar_t buf[64] = {};
        DWORD bufSize = sizeof(buf);
        DWORD type = 0;
        res = RegQueryValueExW(hKey, testVal, nullptr, &type,
                               reinterpret_cast<BYTE*>(buf), &bufSize);
        EXPECT_EQ(res, (LONG)ERROR_SUCCESS);
        EXPECT_TRUE(wcscmp(buf, testData) == 0);

        // Delete value
        res = RegDeleteValueW(hKey, testVal);
        EXPECT_EQ(res, (LONG)ERROR_SUCCESS);

        RegCloseKey(hKey);
    }

    // Clean up test key
    RegDeleteKeyW(HKEY_CURRENT_USER, testKey);
}

static void test_explorer_shell_path_preparation() {
    expectNamed(AURA_HAS_EXPLORER_SHELL_PATH == 1,
                "Explorer Shell path preparation helper exists");
#if AURA_HAS_EXPLORER_SHELL_PATH
    using Platform::Detail::PrepareExplorerPathForShell;

    EXPECT_EQ(PrepareExplorerPathForShell(LR"(\\?\C:\Sound effect\音效.wav)"),
              std::wstring(LR"(C:\Sound effect\音效.wav)"));
    EXPECT_EQ(PrepareExplorerPathForShell(LR"(\\?\UNC\server\share\音效.wav)"),
              std::wstring(LR"(\\server\share\音效.wav)"));
    EXPECT_EQ(PrepareExplorerPathForShell(LR"(\\?\unc\server\share\音效.wav)"),
              std::wstring(LR"(\\server\share\音效.wav)"));
    EXPECT_EQ(PrepareExplorerPathForShell(LR"(C:\Sound effect\音效.wav)"),
              std::wstring(LR"(C:\Sound effect\音效.wav)"));
    EXPECT_EQ(PrepareExplorerPathForShell(LR"(\\server\share\音效.wav)"),
              std::wstring(LR"(\\server\share\音效.wav)"));
    EXPECT_TRUE(PrepareExplorerPathForShell(L"").empty());
#endif
}

static void test_explorer_reveal_contract() {
    const std::string mainSource = readRepoTextFile("src/main.cpp");
    const std::string platformSource = readRepoTextFile("src/platform/system_win32.cpp");

    EXPECT_FALSE(mainSource.empty());
    EXPECT_FALSE(platformSource.empty());
    EXPECT_TRUE(mainSource.find("RevealFileInExplorer(entry.fullPath);") != std::string::npos);
    EXPECT_TRUE(mainSource.find("Platform::OpenInFileManager(filePathUtf8);") != std::string::npos);
    EXPECT_TRUE(mainSource.find("Platform::OpenInFileManager(folderTarget);") != std::string::npos);
    EXPECT_TRUE(mainSource.find("std::system(cmd.c_str())") == std::string::npos);
    expectNamed(mainSource.find("%s (%d)###%s") == std::string::npos,
                "folder tree labels no longer append per-folder file counts");
    expectNamed(mainSource.find("%s (%d)###PlaylistFolder_%s") == std::string::npos,
                "playlist folder tree labels no longer append per-folder file counts");
    EXPECT_TRUE(platformSource.find("SHOpenFolderAndSelectItems") != std::string::npos);
    EXPECT_TRUE(platformSource.find("ILCreateFromPathW") != std::string::npos);
    expectNamed(platformSource.find("PrepareExplorerPathForShell") != std::string::npos,
                "Explorer reveal prepares a Shell-compatible path");
    expectNamed(platformSource.find("ShellExecuteW(nullptr, L\"open\", L\"explorer.exe\", nullptr") == std::string::npos,
                "Explorer reveal does not fall back to unrelated Desktop");
}
#endif

int main() {
    printf("=== TestOpenWith ===\n");
    test_audio_extensions();
    test_path_encoding();
#ifdef _WIN32
    test_registry_roundtrip();
    test_explorer_shell_path_preparation();
    test_explorer_reveal_contract();
#endif
    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
