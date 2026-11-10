// test_avatar_crop.cpp — TestAvatarCrop
// Pure-math unit tests for AvatarCropper. No stb, no filesystem, no GPU.

#include <cstdio>
#include <string>

// Provide stub for GetCustomAvatarPathUtf8's extern dependency
std::string g_appSettingsPathUtf8 = "C:\\test\\settings.ini";

#include "src/AvatarCropper.h"

static int g_failures = 0;
static void check(bool cond, const char* desc) {
    std::printf(cond ? "PASS: %s\n" : "FAIL: %s\n", desc);
    if (!cond) ++g_failures;
}

int main() {
    {
        auto crop = InitCropState(800,600);
        crop.zoom = 100.f;
        ClampCropState(crop);
        check(crop.zoom == 8.f, "zoom is bounded to 8x");
        crop = InitCropState(1,1);
        crop.zoom = 8.f;
        ClampCropState(crop);
        check(ComputeSourceRect(crop).w == 1, "tiny source retains at least one pixel when zoomed");
    }
    // ComputeMinZoom always returns 1.0
    check(ComputeMinZoom(1920, 1080) == 1.0f, "ComputeMinZoom landscape = 1.0");
    check(ComputeMinZoom(256, 256)   == 1.0f, "ComputeMinZoom square = 1.0");
    check(ComputeMinZoom(480, 640)   == 1.0f, "ComputeMinZoom portrait = 1.0");

    // InitCropState: centered, zoom=1
    {
        AvatarCropState s = InitCropState(800, 600);
        check(s.srcW == 800 && s.srcH == 600, "InitCropState stores dimensions");
        check(s.zoom == 1.0f, "InitCropState zoom=1");
        check(s.panX == 0.0f && s.panY == 0.0f, "InitCropState centered");
    }

    // ComputeSourceRect: square image centered
    {
        AvatarCropState s = InitCropState(256, 256);
        AvatarSourceRect r = ComputeSourceRect(s);
        check(r.x == 0 && r.y == 0 && r.w == 256 && r.h == 256,
              "ComputeSourceRect square image centered covers full image");
    }

    // ComputeSourceRect: landscape image, zoom=1, centered -> shorter side crop
    {
        AvatarCropState s = InitCropState(800, 600);  // shorter=600
        AvatarSourceRect r = ComputeSourceRect(s);
        // boxPx=600, cx=400, cy=300 -> x=100, y=0, w=600, h=600
        check(r.w == 600 && r.h == 600, "ComputeSourceRect landscape zoom=1 crop is 600x600");
        check(r.x == 100 && r.y == 0,   "ComputeSourceRect landscape zoom=1 crop x=100 y=0");
    }

    // ComputeSourceRect: landscape image, zoom=2, centered
    {
        AvatarCropState s = InitCropState(800, 600);
        s.zoom = 2.0f;
        AvatarSourceRect r = ComputeSourceRect(s);
        // boxPx=300, cx=400, cy=300 -> x=250, y=150, w=300, h=300
        check(r.w == 300 && r.h == 300, "ComputeSourceRect landscape zoom=2 crop is 300x300");
        check(r.x == 250 && r.y == 150, "ComputeSourceRect landscape zoom=2 crop x=250 y=150");
    }

    // ComputeSourceRect: portrait image, zoom=1, centered -> shorter side crop
    {
        AvatarCropState s = InitCropState(480, 640);  // shorter=480
        AvatarSourceRect r = ComputeSourceRect(s);
        // boxPx=480, cx=240, cy=320 -> x=0, y=80, w=480, h=480
        check(r.w == 480 && r.h == 480, "ComputeSourceRect portrait zoom=1 crop is 480x480");
        check(r.x == 0 && r.y == 80,    "ComputeSourceRect portrait zoom=1 crop x=0 y=80");
    }

    // ComputeSourceRect: degenerate 1x1 — must not crash; rect w/h >= 1
    {
        AvatarCropState s = InitCropState(1, 1);
        AvatarSourceRect r = ComputeSourceRect(s);
        check(r.w >= 1 && r.h >= 1, "ComputeSourceRect 1x1 degenerate: rect w/h >= 1");
    }

    // ClampCropState: pan clamped to valid range
    {
        AvatarCropState s = InitCropState(800, 600);  // shorter=600, at zoom=1 boxPx=600
        s.panX = 9.0f;
        s.panY = 9.0f;
        ClampCropState(s);
        // maxPanX = (800-600)/(2*600) ~= 0.1667; maxPanY = 0
        check(s.panX > 0.0f && s.panX <= 0.1668f, "ClampCropState clamps panX to maxPanX");
        check(s.panY == 0.0f, "ClampCropState clamps panY=0 for landscape with no headroom");
    }

    // ClampCropState: zoom below minimum gets raised
    {
        AvatarCropState s = InitCropState(400, 400);
        s.zoom = 0.1f;
        ClampCropState(s);
        check(s.zoom == 1.0f, "ClampCropState raises zoom below minimum to 1.0");
    }

    // ClampCropState: 1x1 degenerate — pan clamped to 0
    {
        AvatarCropState s = InitCropState(1, 1);
        s.panX = 99.0f; s.panY = 99.0f;
        ClampCropState(s);
        check(s.panX == 0.0f && s.panY == 0.0f, "ClampCropState 1x1 degenerate: pan clamped to 0");
    }

    // GetCustomAvatarPathUtf8: derives path from g_appSettingsPathUtf8
    {
        std::string p = GetCustomAvatarPathUtf8();
        check(p == "C:\\test\\avatar.png", "GetCustomAvatarPathUtf8 strips filename and appends avatar.png");
    }

    // SEMANTIC: ComputeMinZoom must always return 1.0 (cover-not-contain convention)
    check(ComputeMinZoom(1, 1) == 1.0f, "SEMANTIC: ComputeMinZoom is always 1.0 (cover-not-contain convention)");

    if (g_failures == 0)
        std::printf("\nAll TestAvatarCrop checks passed.\n");
    else
        std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
