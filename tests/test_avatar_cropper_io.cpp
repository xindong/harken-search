#include "AvatarCropper.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

std::string g_appSettingsPathUtf8;

namespace {

struct TempDirectory {
    fs::path path;

    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("aris-avatar-io-" + std::to_string(stamp));
        fs::create_directories(path);
    }

    ~TempDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

} // namespace

int main() {
    auto bounded = InitCropState(800,600);
    bounded.zoom=100.f; ClampCropState(bounded);
    if (!Check(bounded.zoom==8.f,"zoom bounded to 8x")) return 1;
    bounded=InitCropState(1,1); bounded.zoom=8.f; ClampCropState(bounded);
    if (!Check(ComputeSourceRect(bounded).w==1,"tiny image crop retains one pixel")) return 1;
    TempDirectory temp;
    const std::vector<uint8_t> pixels = {
        255, 0, 0, 255,    0, 255, 0, 128,
        0, 0, 255, 64,     255, 255, 255, 0,
    };
    const AvatarCropState crop = InitCropState(2, 2);

    const fs::path roundtripPath = temp.path / "roundtrip.png";
    if (!Check(SaveCroppedAvatarPng(pixels, 2, 2, crop, roundtripPath.u8string(), 2),
               "small RGBA crop should save")) return 1;

    int width = 0;
    int height = 0;
    std::vector<uint8_t> loaded;
    if (!Check(LoadImageForCrop(roundtripPath.u8string(), width, height, loaded),
               "saved PNG should load")) return 1;
    if (!Check(width == 2 && height == 2, "roundtrip dimensions changed")) return 1;
    if (!Check(loaded == pixels, "roundtrip RGBA pixels changed")) return 1;

    const fs::path unicodePath = temp.path / fs::u8path(u8"头像😀 crop.png");
    const auto before = [&] { std::ifstream f(roundtripPath, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }();
    auto changedPixels = pixels; changedPixels[0] = 17;
    if (!Check(!SaveCroppedAvatarWithSettings(changedPixels, 2, 2, crop, roundtripPath.u8string(), [] { return false; }, 2),
               "settings failure must reject avatar save")) return 1;
    const auto after = [&] { std::ifstream f(roundtripPath, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }();
    if (!Check(before == after, "settings failure must preserve previous PNG bytes")) return 1;
    const auto firstAvatar = (temp.path / "first.png").u8string();
    if (!Check(!SaveCroppedAvatarWithSettings(pixels, 2, 2, crop, firstAvatar, [] { return false; }, 2) && !fs::exists(fs::u8path(firstAvatar)),
               "first failed save must not leave a custom avatar")) return 1;
    if (!Check(SaveCroppedAvatarWithSettings(pixels, 2, 2, crop, firstAvatar, [] { return true; }, 2),
               "retry after settings recovery must save")) return 1;
    int settingsWrites = 0;
    if (!Check(!SaveCroppedAvatarWithSettings(pixels, 2, 2, crop,
               (temp.path / "missing-parent" / "avatar.png").u8string(),
               [&] { ++settingsWrites; return true; }, 2) && settingsWrites == 0,
               "image write failure must not save settings")) return 1;
    if (!Check(SaveCroppedAvatarWithSettings(changedPixels, 2, 2, crop, roundtripPath.u8string(),
               [&] { ++settingsWrites; return true; }, 2) && settingsWrites == 1,
               "successful replacement must save settings once")) return 1;
    loaded.clear();
    if (!Check(LoadImageForCrop(roundtripPath.u8string(), width, height, loaded) && loaded == changedPixels,
               "successful replacement must persist new pixels")) return 1;
    for (const auto& entry : fs::directory_iterator(temp.path)) {
        if (!Check(entry.path().filename().u8string().find(".pending-") == std::string::npos &&
                   entry.path().filename().u8string().find(".backup-") == std::string::npos,
                   "completed saves must clean temporary image files")) return 1;
    }
    if (!Check(SaveCroppedAvatarPng(pixels, 2, 2, crop, unicodePath.u8string(), 2),
               "Unicode destination should save")) return 1;
    loaded.clear();
    if (!Check(LoadImageForCrop(unicodePath.u8string(), width, height, loaded),
               "Unicode source should load")) return 1;
    if (!Check(width == 2 && height == 2 && loaded == pixels,
               "Unicode roundtrip changed image")) return 1;

    if (!Check(!LoadImageForCrop((temp.path / "missing.png").u8string(), width, height, loaded),
               "missing source should fail")) return 1;
    const fs::path corruptPath = temp.path / "corrupt.png";
    {
        std::ofstream corrupt(corruptPath, std::ios::binary);
        corrupt << "not an image";
    }
    if (!Check(!LoadImageForCrop(corruptPath.u8string(), width, height, loaded),
               "corrupt source should fail")) return 1;

    AvatarCropState invalidCrop = crop;
    invalidCrop.srcW = 0;
    invalidCrop.srcH = 0;
    if (!Check(!SaveCroppedAvatarPng(pixels, 2, 2, invalidCrop,
                                     (temp.path / "invalid.png").u8string(), 2),
               "invalid crop should fail")) return 1;
    if (!Check(!SaveCroppedAvatarPng(pixels, 2, 2, crop, temp.path.u8string(), 2),
               "unwritable directory destination should fail")) return 1;

    std::cout << "PASS: Avatar cropper PAL I/O contracts\n";
    return 0;
}
