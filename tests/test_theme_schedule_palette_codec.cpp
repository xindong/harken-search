#include "ThemeSchedulePaletteCodec.h"
#include "ThemeEditorSchema.h"
#include "nlohmann_json.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace harken::theme_schedule;
static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    ThemeColorScheme night;
    night.toolbarAccent = ImVec4(.123456f, .42f, .78f, .91f);
    night.sidebarIconActive = ImVec4(.3f, .4f, .5f, .6f);
    night.wwiseDiffNew = ImVec4(.2f, .6f, .7f, .8f);
    ThemeColorScheme decoded;
    std::string error;
    const auto encoded = EncodePalette(night);
    Check(DecodePalette(encoded, Period::Night, decoded, error), "night round trip");
    Check(PalettesEqual(night, decoded), "preset identity survives palette round trip");
    size_t count = 0;
    const auto* fields = GetThemeEditorItems(count);
    for (size_t i = 0; i < count; ++i) {
        const auto a = night.*(fields[i].member), b = decoded.*(fields[i].member);
        Check(a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w, fields[i].key);
        auto edited = decoded;
        (edited.*(fields[i].member)).w = b.w == 1.f ? .5f : 1.f;
        Check(!PalettesEqual(night, edited), "edited field must not keep the stock preset name");
    }
    Check(decoded.sidebarIconActive.w == .6f && decoded.wwiseDiffNew.z == .7f, "non-editor colors survive");
    auto hiddenEdit = decoded;
    hiddenEdit.wwiseDiffNew.x = .8f;
    Check(!PalettesEqual(night, hiddenEdit), "non-editor field must affect preset identity");
    Check(!DecodePalette(encoded, Period::Day, decoded, error), "dark palette rejected for day");
    const auto before = decoded.toolbarAccent.x;
    for (const char* bad : {"{}", "{broken", "[]", "null"}) {
        Check(!DecodePalette(bad, Period::Night, decoded, error), "bad JSON rejected");
        Check(decoded.toolbarAccent.x == before, "failed load leaves destination intact");
    }
    auto json = nlohmann::json::parse(encoded);
    json["colors"]["resultsBG"] = {0.1, 0.1, 4.0, 1.0};
    Check(!DecodePalette(json.dump(), Period::Night, decoded, error), "out-of-range channel rejected");
    json = nlohmann::json::parse(encoded);
    json["colors"].erase("toolbarAccent");
    Check(!DecodePalette(json.dump(), Period::Night, decoded, error), "partial palette rejected");
    auto day = night;
    day.mainWindowBG = day.resultsBG = day.dialogBG = day.waveformBG = ImVec4(.94f,.95f,.97f,1);
    Check(DecodePalette(EncodePalette(day), Period::Day, decoded, error), "light palette accepted for day");
    Check(!DecodePalette(EncodePalette(day), Period::Night, decoded, error), "light palette rejected for night");
    day.toolbarAccent.x = std::numeric_limits<float>::quiet_NaN();
    Check(!ValidatePalette(day, Period::Day, error), "nonfinite value rejected before writing");
    std::cout << "Full palette round-trip, category and corrupt input checks passed\n";
}
