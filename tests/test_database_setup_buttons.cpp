// First-library buttons: the label must never be clipped, and three labels that
// do not fit side by side must fall back to one button per line.
//
// Runs headlessly against the vendored ImGui so the measured text sizes and
// frame height are the real ones the dialog uses.

#include "ui/DatabaseSetupButtonLayout.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

bool NearlyEqual(float left, float right) {
    return std::fabs(left - right) < 0.01f;
}

void ScaleLikeTheApp(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    // The app re-bakes the style for the composed DPI × interface factor and
    // multiplies fonts through FontScaleDpi/FontScaleMain.
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FontScaleMain = 1.0f;
    // ImGui re-derives the current font size at NewFrame, so the new scale only
    // reaches CalcTextSize/GetFrameHeight across a frame boundary.
    ImGui::EndFrame();
    ImGui::NewFrame();
}

void CheckScale(float scale, const char* label) {
    ScaleLikeTheApp(scale);
    const float height = aura::ui::setup::ButtonHeight(scale, ImGui::GetFrameHeight());
    const float textHeight = ImGui::CalcTextSize("Create", nullptr, true).y;
    const float framePaddingY = ImGui::GetStyle().FramePadding.y;

    char message[160];
    std::snprintf(message, sizeof(message), "%s: height keeps the design floor", label);
    Check(height >= 32.0f * scale - 0.01f, message);
    std::snprintf(message, sizeof(message), "%s: height leaves room below the glyphs", label);
    Check(height >= textHeight + framePaddingY * 2.0f - 0.01f, message);
    std::snprintf(message, sizeof(message), "%s: height equals the line box when it is taller", label);
    Check(height >= ImGui::GetFrameHeight() - 0.01f, message);
}

} // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(3840.0f, 2019.0f);
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int textureWidth = 0;
    int textureHeight = 0;
    // Build the atlas the way a renderer backend would; this test has no backend.
    io.Fonts->GetTexDataAsRGBA32(&pixels, &textureWidth, &textureHeight);
    ImGui::NewFrame();

    CheckScale(1.0f, "100%");
    const float textAt100 = ImGui::CalcTextSize("Create", nullptr, true).y;
    CheckScale(2.0f, "200% DPI");
    const float textAt200 = ImGui::CalcTextSize("Create", nullptr, true).y;
    CheckScale(2.5f, "200% DPI × 125%");
    const float textAt250 = ImGui::CalcTextSize("Create", nullptr, true).y;
    // Guard the measurement itself: if the font scale never reached the atlas
    // these numbers would be identical and every height assertion would be fake.
    Check(textAt200 > textAt100 * 1.8f, "200% DPI really enlarges the measured glyphs");
    Check(textAt250 > textAt100 * 2.3f, "250% really enlarges the measured glyphs");

    // 4K at 200% DPI with the interface at 125%: the dialog content is wide, so
    // the three buttons stay on one row and share the surplus evenly.
    ScaleLikeTheApp(2.5f);
    const ImVec2 createText = ImGui::CalcTextSize("Create", nullptr, true);
    const ImVec2 openText = ImGui::CalcTextSize("Open Database...", nullptr, true);
    const ImVec2 exitText = ImGui::CalcTextSize("Exit", nullptr, true);
    const float padX = ImGui::GetStyle().FramePadding.x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float frameHeight = ImGui::GetFrameHeight();

    const auto wide = aura::ui::setup::ResolveRow(
        1120.0f, spacing, createText, openText, exitText, padX, 2.5f, frameHeight);
    Check(!wide.stacked, "wide dialog keeps one row");
    Check(NearlyEqual(wide.create.y, wide.open.y) && NearlyEqual(wide.open.y, wide.exit.y),
          "one row: all three buttons share one height");
    Check(wide.exit.x >= 72.0f * 2.5f - 0.01f, "exit keeps its scaled minimum width");
    Check(NearlyEqual(wide.create.x + wide.open.x + wide.exit.x + spacing * 2.0f, 1120.0f),
          "one row: widths fill the content width");
    Check(wide.create.x >= createText.x + padX * 2.0f - 0.01f, "create label fits");
    Check(wide.open.x >= openText.x + padX * 2.0f - 0.01f, "open label fits");

    // Narrow client area: the measured labels no longer fit, so every button
    // takes a full row instead of squeezing the text.
    const auto narrow = aura::ui::setup::ResolveRow(
        400.0f, spacing, createText, openText, exitText, padX, 2.5f, frameHeight);
    Check(narrow.stacked, "narrow dialog stacks the buttons");
    Check(NearlyEqual(narrow.create.x, 400.0f) && NearlyEqual(narrow.open.x, 400.0f) &&
              NearlyEqual(narrow.exit.x, 400.0f),
          "stacked: each button uses the full width");
    Check(NearlyEqual(narrow.create.y, narrow.open.y) && NearlyEqual(narrow.open.y, narrow.exit.y),
          "stacked: all three buttons share one height");

    // One very wide label must not be squeezed below its own minimum just
    // because the equal-share width would overflow the dialog.
    const auto asymmetric = aura::ui::setup::ResolveRow(
        720.0f, spacing, ImVec2(300.0f, 30.0f), ImVec2(120.0f, 30.0f),
        exitText, padX, 1.0f, frameHeight);
    Check(!asymmetric.stacked, "asymmetric labels still use one row when they fit");
    Check(asymmetric.create.x >= 300.0f + padX * 2.0f - 0.01f, "wide label keeps its measured width");
    Check(asymmetric.open.x >= 120.0f + padX * 2.0f - 0.01f, "narrow label keeps its measured width");
    Check(asymmetric.create.x + asymmetric.open.x + asymmetric.exit.x + spacing * 2.0f <= 720.0f + 0.01f,
          "asymmetric row stays inside the dialog");

    // A pinned 100% interface scale must not change today's geometry.
    ScaleLikeTheApp(1.0f);
    const auto baseline = aura::ui::setup::ResolveRow(
        560.0f, ImGui::GetStyle().ItemSpacing.x,
        ImGui::CalcTextSize("Create", nullptr, true),
        ImGui::CalcTextSize("Open Database...", nullptr, true),
        ImGui::CalcTextSize("Exit", nullptr, true),
        ImGui::GetStyle().FramePadding.x, 1.0f, ImGui::GetFrameHeight());
    Check(NearlyEqual(baseline.create.y, 32.0f) || baseline.create.y >= ImGui::GetFrameHeight(),
          "100%: height stays at the design value");
    Check(!baseline.stacked, "100%: the three buttons stay on one row");

    ImGui::EndFrame();
    ImGui::DestroyContext();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
