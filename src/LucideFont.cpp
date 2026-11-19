#include "HarkenSearchIcons.h"
#include "platform/executable_path.h"
#include "imgui.h"

#include <filesystem>

namespace aura::ui {

void MergeLucideFont(ImFontAtlas* atlas, float fontPixelSize) {
    if (!atlas) return;

    static const ImWchar kGlyphRange[] = {0xE042, 0xE55E, 0};

    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = fontPixelSize;
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.FontDataOwnedByAtlas = true;
    cfg.Flags |= ImFontFlags_NoLoadError;

    const auto executable = Platform::GetExecutablePath();
    if (executable.status == Platform::ExecutablePathStatus::Success) {
        const auto directory = std::filesystem::u8path(executable.pathUtf8).parent_path();
        if (directory.is_absolute()) {
            const auto path = (directory / "lucide.ttf").u8string();
            if (atlas->AddFontFromFileTTF(path.c_str(), fontPixelSize, &cfg, kGlyphRange)) {
                return;
            }
        }
    }

    (void)atlas->AddFontFromFileTTF("thirdparty/lucide/lucide.ttf", fontPixelSize, &cfg, kGlyphRange);
}

}
