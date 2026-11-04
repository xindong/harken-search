#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../thirdparty/stb_image_write.h"
#include "../thirdparty/stb_image_resize2.h"
#include "../thirdparty/stb_image.h"  // STB_IMAGE_IMPLEMENTATION is in stb_image_impl.cpp
#include "AvatarCropper.h"
#include "platform/filesystem.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <string_view>

bool LoadImageForCrop(const std::string& pathUtf8, int& outW, int& outH,
                      std::vector<uint8_t>& outPixels) {
    const auto bytes = Platform::FileSystem::ReadFileBytes(pathUtf8);
    if (bytes.empty() || bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    int channels = 0;
    unsigned char* data = stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()), &outW, &outH, &channels, 4);
    if (!data) return false;
    outPixels.assign(data, data + outW * outH * 4);
    stbi_image_free(data);
    return true;
}

bool SaveCroppedAvatarPng(const std::vector<uint8_t>& srcPixels,
                           int srcW, int srcH,
                           const AvatarCropState& crop,
                           const std::string& destPathUtf8,
                           int outputSize) {
    const AvatarSourceRect r = ComputeSourceRect(crop);
    const int rx = std::max(0, std::min(r.x, srcW - 1));
    const int ry = std::max(0, std::min(r.y, srcH - 1));
    const int rw = std::min(r.w, srcW - rx);
    const int rh = std::min(r.h, srcH - ry);
    if (rw <= 0 || rh <= 0) return false;

    std::vector<uint8_t> region(rw * rh * 4);
    for (int row = 0; row < rh; ++row) {
        const uint8_t* src = srcPixels.data() + ((ry + row) * srcW + rx) * 4;
        uint8_t* dst = region.data() + row * rw * 4;
        std::copy(src, src + rw * 4, dst);
    }

    std::vector<uint8_t> resized(outputSize * outputSize * 4);
    stbir_resize_uint8_linear(
        region.data(),  rw,         rh,         rw * 4,
        resized.data(), outputSize, outputSize, outputSize * 4,
        STBIR_RGBA);

    Platform::FileSystem::StreamWriter writer;
    if (!writer.Open(destPathUtf8, false, true)) return false;

    struct WriteContext {
        Platform::FileSystem::StreamWriter* writer = nullptr;
        bool writeOk = true;
    } context{&writer, true};

    auto writeCallback = [](void* ctx, void* data, int size) {
        auto& state = *static_cast<WriteContext*>(ctx);
        if (!state.writeOk || size < 0) {
            state.writeOk = false;
            return;
        }
        auto& writer = *state.writer;
        state.writeOk = writer.Write(std::string_view(
            static_cast<const char*>(data), static_cast<size_t>(size)));
    };
    const bool encoded = stbi_write_png_to_func(
        writeCallback, &context, outputSize, outputSize, 4,
        resized.data(), outputSize * 4) != 0;
    const bool writeOk = context.writeOk;
    const bool flushed = writer.Flush();
    writer.Close();
    return encoded && writeOk && flushed;
}

bool SaveCroppedAvatarWithSettings(const std::vector<uint8_t>& srcPixels,
                                  int srcW, int srcH, const AvatarCropState& crop,
                                  const std::string& destPathUtf8,
                                  const std::function<bool()>& saveSettings,
                                  int outputSize) {
    namespace fs = Platform::FileSystem;
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto staged = destPathUtf8 + ".pending-" + suffix;
    const auto backup = destPathUtf8 + ".backup-" + suffix;
    const bool hadImage = fs::Exists(destPathUtf8);
    if (hadImage && !fs::CopyFile(destPathUtf8, backup, false)) return false;
    if (!SaveCroppedAvatarPng(srcPixels, srcW, srcH, crop, staged, outputSize) ||
        !fs::AtomicFileReplace(destPathUtf8, staged)) {
        fs::RemoveFile(staged);
        if (hadImage) fs::RemoveFile(backup);
        return false;
    }
    if (!saveSettings()) {
        // Keep the backup if restoration fails so the old bytes remain recoverable.
        if (hadImage) fs::AtomicFileReplace(destPathUtf8, backup);
        else fs::RemoveFile(destPathUtf8);
        return false;
    }
    if (hadImage) fs::RemoveFile(backup);
    return true;
}
