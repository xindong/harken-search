#include "imgui.h"
#include <cmath>
#include <cstdio>
#include "ui/TransportContrast.h"
#include <initializer_list>
int main() {
    using namespace TransportContrast;
    const ImVec4 black(0, 0, 0, 1), white(1, 1, 1, 1);
    if (std::fabs(Ratio(white, black) - 21.0) > .00001) return 1;
    if (std::fabs(Ratio(ImVec4(1, 1, 1, 0), black) - 1.0) > .00001) return 2;
    const ImVec4 readable(.92f, .95f, .97f, 1);
    const ImVec4 unchanged = ForSurface(readable, black, 4.5);
    if (unchanged.x != readable.x || unchanged.y != readable.y || unchanged.z != readable.z) return 3;
    const ImVec4 backgrounds[] = {
        ImVec4(14/255.f,61/255.f,72/255.f,1), ImVec4(.88f,.81f,.84f,1),
        ImVec4(.035f,.055f,.075f,1), ImVec4(.13f,.17f,.21f,1),
        ImVec4(.44f,.44f,.44f,1), white, black};
    const ImVec4 foregrounds[] = {
        ImVec4(1/255.f,64/255.f,95/255.f,.48f), ImVec4(.14f,.43f,.41f,1),
        ImVec4(.72f,.36f,.48f,1), ImVec4(1,1,1,.48f), ImVec4(.4f,.4f,.4f,.43f)};
    for (const auto& bg : backgrounds) for (const auto& fg : foregrounds) {
        for (double floor : {3.0, 4.5}) {
            const auto fixed = ForSurface(fg, bg, floor);
            if (Ratio(Quantize(fixed), Quantize(bg)) < floor) {
                std::puts("FAIL: real 8-bit draw color misses contrast floor"); return 4;
            }
        }
    }
    // An optional safety margin may be impossible on a mid-gray surface.
    // Return the most readable endpoint, never an uninitialized color.
    const ImVec4 mid = Quantize(ImVec4(.46f,.46f,.46f,1));
    const auto fallback = ForSurface(ImVec4(.46f,.46f,.46f,.48f), mid, 7.0);
    if (std::fabs(Ratio(Quantize(fallback), mid) - std::max(Ratio(white, mid), Ratio(black, mid))) > .00001) return 5;
    std::puts("Transport contrast preserves readable colors and repairs low contrast after quantization");
    return 0;
}
