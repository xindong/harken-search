#include "AppState.h"
#include "Theme.h"
#include "BuiltinThemeScheme.h"
#include "ui/EmptyStatePalette.h"
#include "ui/TransportContrast.h"
#include <cstdio>

int main() {
    using namespace TransportContrast;
    int failures = 0;
    for (int id : {0, 1, 2, 3, 10}) {
        const auto theme = MakeBuiltinScheme(static_cast<AppTheme>(id));
        // Actual CenterFileList host surface, including its theme blend.
        const float luma=theme.resultsBG.x*.299f+theme.resultsBG.y*.587f+theme.resultsBG.z*.114f;
        const float t=luma>=.72f ? .06f : .02f;
        const ImVec4 host(theme.resultsBG.x+(theme.mainWindowBG.x-theme.resultsBG.x)*t,
            theme.resultsBG.y+(theme.mainWindowBG.y-theme.resultsBG.y)*t,
            theme.resultsBG.z+(theme.mainWindowBG.z-theme.resultsBG.z)*t,1);
        const auto p = EmptyStatePalette::Make(theme,Quantize(host));
        auto check = [&](const char* name, ImVec4 fg, ImVec4 bg) {
            double ratio = Ratio(Quantize(fg), Quantize(bg));
            std::printf("theme=%d %s %.4f\n", id, name, ratio);
            if (ratio < 4.5) ++failures;
        };
        for (auto surface : p.surfaces) {
            check("title", p.titleColor, surface);
            check("body", p.bodyColor, surface);
            check("hint", p.hintColor, surface);
            for (auto bg : {p.primaryButtonBg,p.primaryButtonHover,p.primaryButtonActive})
                check("new",p.primaryButtonText,Over(Quantize(bg),surface));
            for (auto bg : {p.secondaryButtonBg,p.secondaryButtonHover,p.secondaryButtonActive}) {
                check("open",p.secondaryButtonText,Over(Quantize(bg),surface));
                const auto edgeRatio=Ratio(Over(Quantize(bg),surface),surface);
                std::printf("theme=%d open-button-surface %.4f\n",id,edgeRatio);
                if(edgeRatio<3.0) ++failures;
            }
        }
    }
    std::printf("contrast failures: %d\n", failures);
    return failures ? 1 : 0;
}
