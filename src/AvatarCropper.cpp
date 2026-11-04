#include "AvatarCropper.h"
#include <algorithm>
#include <string>

extern std::string g_appSettingsPathUtf8;

std::string GetCustomAvatarPathUtf8() {
    if (g_appSettingsPathUtf8.empty()) return {};
    const auto sep = g_appSettingsPathUtf8.rfind('\\');
    const std::string dir = (sep != std::string::npos)
        ? g_appSettingsPathUtf8.substr(0, sep)
        : g_appSettingsPathUtf8;
    return dir + "\\avatar.png";
}

float ComputeMinZoom(int /*srcW*/, int /*srcH*/) {
    return 1.0f;
}

AvatarCropState InitCropState(int srcW, int srcH) {
    AvatarCropState s;
    s.srcW = srcW;
    s.srcH = srcH;
    s.zoom = ComputeMinZoom(srcW, srcH);
    return s;
}

AvatarSourceRect ComputeSourceRect(const AvatarCropState& crop) {
    const int   shorter = std::min(crop.srcW, crop.srcH);
    const float boxPx   = shorter / crop.zoom;
    const float cx      = crop.srcW * 0.5f + crop.panX * shorter;
    const float cy      = crop.srcH * 0.5f + crop.panY * shorter;
    AvatarSourceRect r;
    r.w = static_cast<int>(boxPx + 0.5f);
    r.h = r.w;
    r.x = static_cast<int>(cx - boxPx * 0.5f + 0.5f);
    r.y = static_cast<int>(cy - boxPx * 0.5f + 0.5f);
    return r;
}

void ClampCropState(AvatarCropState& crop) {
    const float maxZoom = static_cast<float>(std::max(1, std::min(8, std::min(crop.srcW,crop.srcH))));
    crop.zoom = std::clamp(crop.zoom, ComputeMinZoom(crop.srcW, crop.srcH), maxZoom);
    const int   shorter  = std::min(crop.srcW, crop.srcH);
    const float boxPx    = shorter / crop.zoom;
    const float maxPanX  = (crop.srcW - boxPx) / (2.0f * shorter);
    const float maxPanY  = (crop.srcH - boxPx) / (2.0f * shorter);
    crop.panX = std::max(-maxPanX, std::min(maxPanX, crop.panX));
    crop.panY = std::max(-maxPanY, std::min(maxPanY, crop.panY));
}
