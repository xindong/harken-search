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
