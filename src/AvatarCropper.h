#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

// @stage7-contract
// API contract: crop helpers are pure value math with no I/O/GPU side effects.
// ComputeSourceRect consumes the supplied state without retaining references;
// ClampCropState preserves the normalized bounds documented by the fields.

// Crop state (pure math, no I/O or GPU dependency)
struct AvatarCropState {
    float panX = 0.0f;  // normalized offset; 0 = centered; clamped by ClampCropState
    float panY = 0.0f;
    float zoom = 1.0f;  // 1.0 = shorter side fills crop box; > 1.0 = zoomed in
    int   srcW = 0;
    int   srcH = 0;
};

struct AvatarSourceRect { int x, y, w, h; };

// Returns 1.0f — shorter side fills crop box at zoom=1, always covers
float            ComputeMinZoom(int srcW, int srcH);

// Returns pixel rect in source image coords that the current crop maps to
AvatarSourceRect ComputeSourceRect(const AvatarCropState& crop);

// Clamps panX/panY so crop box stays entirely within image bounds
void             ClampCropState(AvatarCropState& crop);

// Initializes crop state for a freshly loaded image (centered, zoom=1)
AvatarCropState  InitCropState(int srcW, int srcH);

// Production: %LOCALAPPDATA%\HarkenSearch\avatar.png
// Dev mode:   <runtimeRoot>\avatar.png
// Derived from g_appSettingsPathUtf8 defined in WindowsMain.cpp
std::string GetCustomAvatarPathUtf8();

// Loads JPEG/PNG/BMP/TGA into RGBA8 pixels via stb_image.
bool LoadImageForCrop(const std::string& pathUtf8,
                      int& outW, int& outH,
                      std::vector<uint8_t>& outPixels);

// Bilinear-resizes the crop region to outputSize x outputSize and writes PNG.
bool SaveCroppedAvatarPng(const std::vector<uint8_t>& srcPixels,
                           int srcW, int srcH,
                           const AvatarCropState& crop,
                           const std::string& destPathUtf8,
                           int outputSize = 256);

// Commit the image and settings together; retain the previous image on failure.
bool SaveCroppedAvatarWithSettings(const std::vector<uint8_t>& srcPixels,
                                  int srcW, int srcH, const AvatarCropState& crop,
                                  const std::string& destPathUtf8,
                                  const std::function<bool()>& saveSettings,
                                  int outputSize = 256);
