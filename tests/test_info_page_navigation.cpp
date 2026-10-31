#include "InfoPageNavigation.h"
#include <iostream>
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { std::cerr << "FAIL: " << label << "\n"; ++failures; }
    };
    InfoPageNavigation pages;
    check(pages.selected == 1, "first launch opens artwork");
    check(pages.Select(InfoPage::Playback, true) && pages.selected == 3, "switch to playback");
    check(pages.SetVisible(InfoPage::Playback, false, true) && pages.selected == 0, "hiding active page selects first visible page");
    check(!pages.IsVisible(InfoPage::Playback, true), "hidden page is absent");
    check(!pages.Select(InfoPage::Playback, true), "hidden page cannot be selected");
    check(pages.SetVisible(InfoPage::Playback, true, true), "restore hidden page");
    pages.visibleMask = 2; pages.selected = 1;
    check(!pages.SetVisible(InfoPage::Artwork, false, true) && pages.visibleMask == 2, "last visible page cannot be hidden");
    pages.visibleMask = 4; pages.selected = 2; pages.Normalize(false);
    check(pages.selected == 1 && pages.IsVisible(InfoPage::Artwork, false), "release restores usable page from diagnostics-only settings");
    check(!pages.IsVisible(InfoPage::Transfer, false) && !pages.Select(InfoPage::Transfer, false), "release omits transfer placeholder");
    pages.visibleMask = 0; pages.selected = 99; pages.Normalize(true);
    check(pages.visibleMask == 2 && pages.selected == 1, "invalid saved selection recovers to artwork");
    pages.visibleMask = 9; pages.selected = 3; pages.Normalize(false);
    check(pages.visibleMask == 9 && pages.selected == 3 && pages.VisibleCount(false) == 2, "saved choice survives reload");
    pages.visibleMask = -1; pages.Normalize(true);
    check(pages.visibleMask == 15, "unknown saved bits are discarded");
    check(!pages.SetVisible(static_cast<InfoPage>(88), false, true), "invalid page cannot shift outside mask");
    std::cout << failures << " failures\n";
    return failures ? 1 : 0;
}
