#include "ui/DiagnosticOverlayLayout.h"

#include <cstdio>

namespace {

using namespace harken::diagnostic_overlay::layout;

int failures = 0;

void Check(bool ok, const char* message) {
    if (ok) return;
    ++failures;
    std::printf("FAIL %s\n", message);
}

void TestDefaultAndDpi() {
    const auto normal = FitPlacement(kDefaultPlacement, WindowBounds({1200.0f, 800.0f}, 1.0f));
    Check(normal.size.width == 420.0f && normal.size.height == 304.0f &&
              normal.inset.right == 10.0f && normal.inset.top == 10.0f,
          "default window is 420 x 304 and inset from the top right");
    const auto highDpi = FitPlacement(kDefaultPlacement, WindowBounds({1500.0f, 900.0f}, 2.0f));
    Check(highDpi.size.width == 840.0f && highDpi.size.height == 608.0f &&
              highDpi.inset.right == 20.0f && highDpi.inset.top == 20.0f,
          "DPI scales both window dimensions and insets");
}

void TestSmallHostPreservesPreferredSize() {
    Placement preferred{{620.0f, 400.0f}, {24.0f, 16.0f}};
    const auto bounds = WindowBounds({250.0f, 170.0f}, 1.0f);
    const auto fitted = FitPlacement(preferred, bounds);
    Check(bounds.scale < 1.0f && fitted.size.width > 0.0f && fitted.size.height > 0.0f &&
              fitted.inset.right >= 0.0f && fitted.size.width + fitted.inset.right <= 250.0f &&
              fitted.inset.top >= 0.0f && fitted.size.height + fitted.inset.top <= 170.0f,
          "small host fits the whole window including the top-right close button");
    RememberResize(preferred, fitted, fitted, bounds.scale);
    const auto restored = FitPlacement(preferred, WindowBounds({1200.0f, 800.0f}, 1.0f));
    Check(restored.size.width == 620.0f && restored.size.height == 400.0f &&
              restored.inset.right == 24.0f && restored.inset.top == 16.0f,
          "automatic fitting does not overwrite the preferred size or position");
}

void TestAnchoredResizeLimits() {
    const auto bounds = WindowBounds({1200.0f, 800.0f}, 1.0f);
    const auto placed = FitPlacement(kDefaultPlacement, bounds);
    const auto lowerLeft = ResizeMaximum(bounds, placed, true, false);
    const auto lowerRight = ResizeMaximum(bounds, placed, false, false);
    const auto upperEdge = ResizeMaximum(bounds, placed, false, true);
    Check(lowerLeft.width == 1190.0f && lowerLeft.height == 790.0f,
          "lower left grows toward the left and bottom without moving the top right");
    Check(lowerRight.width == 430.0f && lowerRight.height == 790.0f,
          "lower right grows only into the space to its right and below");
    Check(upperEdge.height == 314.0f, "resizing the top edge keeps the bottom fixed");
}

void TestResizeRemembersOnlyChangedAxes() {
    Placement preferred{{600.0f, 400.0f}, {10.0f, 10.0f}};
    const Placement fitted{{300.0f, 170.0f}, {5.0f, 0.0f}};
    const Placement resized{{320.0f, 170.0f}, {4.0f, 0.0f}};
    RememberResize(preferred, fitted, resized, 0.5f);
    Check(preferred.size.width == 640.0f && preferred.size.height == 400.0f &&
              preferred.inset.right == 8.0f && preferred.inset.top == 10.0f,
          "width-only resizing preserves the unclamped preferred height and top inset");
}

void TestTypographyGrowsInStepsWithRoomOnBothAxes() {
    const float normal = FontScale({420.0f, 304.0f});
    const float bigger = FontScale({540.0f, 380.0f});
    const float largest = FontScale({700.0f, 500.0f});
    Check(normal == 1.0f && FontScale({700.0f, 304.0f}) == normal &&
              FontScale({420.0f, 500.0f}) == normal,
          "extra space on only one axis goes to content");
    Check(bigger > normal && largest > bigger && largest <= 1.25f && FontScale({580.0f, 400.0f}) == bigger,
          "two-axis growth selects stable, modest font tiers");
}

}

int main() {
    TestDefaultAndDpi();
    TestSmallHostPreservesPreferredSize();
    TestAnchoredResizeLimits();
    TestResizeRemembersOnlyChangedAxes();
    TestTypographyGrowsInStepsWithRoomOnBothAxes();
    return failures != 0;
}
