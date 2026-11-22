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
        const auto original = fields[i].Color(night);
        auto color = fields[i].Color(decoded);
        Check(original.x == color.x && original.y == color.y && original.z == color.z && original.w == color.w, fields[i].key);
        auto edited = decoded;
        color.w = color.w == 1.f ? .5f : 1.f;
        fields[i].SetColor(edited, color);
        Check(!PalettesEqual(night, edited), "edited field must not keep the stock preset name");
    }
    Check(decoded.sidebarIconActive.w == .6f && decoded.wwiseDiffNew.z == .7f, "non-editor colors survive");
    auto hiddenEdit = decoded;
    hiddenEdit.wwiseDiffNew.x = .8f;
    Check(!PalettesEqual(night, hiddenEdit), "non-editor field must affect preset identity");
    Check(DecodePalette(encoded, Period::Day, decoded, error), "dark palette accepted for day");
    Check(PalettesEqual(night, decoded), "day slot preserves the chosen dark palette");
    auto wwise = nlohmann::json::parse(encoded);
    Check(!wwise["colors"].contains("wwiseLabelText") && !wwise["colors"].contains("wwiseBodyText"),
          "following Wwise colors do not change the legacy palette document");
    wwise["colors"]["wwiseLabelText"] = {0.25f, 0.5f, 0.75f, 0.f};
    wwise["colors"]["wwiseBodyText"] = {1.f, 0.5f, 0.f, 1.f};
    for (const auto period : {Period::Day, Period::Night}) {
        Check(DecodePalette(wwise.dump(), period, decoded, error), "Wwise text overrides load in both palette slots");
        Check(!PalettesEqual(night, decoded), "explicit Wwise override differentiates a palette from its preset");
        Check(nlohmann::json::parse(EncodePalette(decoded))["colors"] == wwise["colors"],
              "Wwise text overrides round trip without changing the other palette colors");
    }
    wwise["colors"]["wwiseLabelText"][3] = -0.1f;
    Check(!DecodePalette(wwise.dump(), Period::Night, decoded, error), "invalid Wwise override is rejected");
    Check(DecodePalette(encoded, Period::Night, decoded, error) && PalettesEqual(night, decoded),
          "loading a legacy slot restores follow mode");
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
    Check(DecodePalette(EncodePalette(day), Period::Night, decoded, error), "light palette accepted for night");
    Check(PalettesEqual(day, decoded), "night slot preserves the chosen light palette");
    day.toolbarAccent.x = std::numeric_limits<float>::quiet_NaN();
    Check(!ValidatePalette(day, Period::Day, error), "nonfinite value rejected before writing");
    std::cout << "Full palette round-trip, unrestricted periods and corrupt input checks passed\n";
}
