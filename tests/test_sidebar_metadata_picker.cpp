#include "SidebarMetadataPicker.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) std::printf("FAIL: %s\n", message);
    return condition;
}

} // namespace

int main() {
    using namespace harken::sidebar;

    MetadataPickerDraft draft;
    draft.Sync("Adele");
    draft.Open();
    if (!Check(draft.IsOpen() && draft.DisplayText() == "Adele" && draft.Query().empty(),
               "opening a picker must show the applied value while listing every name")) return 1;

    draft.Edit("be");
    draft.Sync("Adele");
    if (!Check(draft.IsOpen() && draft.Query() == "be" && draft.Committed() == "Adele",
               "typing must search the list without applying the draft")) return 1;
    if (!Check(NameContains("Bebe Rexha", draft.Query()) &&
               NameContains("Benjamin", draft.Query()) &&
               !NameContains("Adele", draft.Query()),
               "name search must use case-insensitive containment")) return 1;
    if (!Check(NameContains("乐队贝壳", "贝壳") && !NameContains("Bebe Rexha", "bebe rexa"),
               "UTF-8 names match literally and typos do not match")) return 1;
    draft.ResetQuery();
    if (!Check(draft.Query().empty() && draft.DisplayText() == "Adele",
               "initial jumping must restore the full list without changing the applied filter")) return 1;

    draft.Cancel();
    if (!Check(!draft.IsOpen() && draft.DisplayText() == "Adele" && draft.Committed() == "Adele",
               "outside close must restore the applied filter")) return 1;
    draft.Open();
    draft.Edit("be");
    if (!Check(draft.Select("Benjamin") && !draft.IsOpen() && draft.Committed() == "Benjamin",
               "picking one name commits that exact name")) return 1;
    draft.Open();
    if (!Check(draft.Query().empty(), "reopening must show the full list")) return 1;
    if (!Check(draft.Select("") && draft.Committed().empty(), "the All row must clear the filter")) return 1;
    draft.Open();
    draft.Edit("old");
    draft.Sync("changed by another tab");
    if (!Check(!draft.IsOpen() && draft.DisplayText() == "changed by another tab",
               "an external filter change must invalidate a stale draft")) return 1;

    const std::vector<std::string> names = {"Adele", "Amber", "Bebe Rexha", "Benjamin", "Björk", "Zedd"};
    if (!Check(FirstInitialIndex(names, 'b') == 2 && FirstInitialIndex(names, 'Z') == 5,
               "a letter jumps to the first name with that initial")) return 1;
    if (!Check(FirstInitialIndex(names, 'Q') == -1,
               "an absent initial must leave the current scroll alone")) return 1;

    if (!Check(PickerDisplayName("Fusehive-\x1a\x1a", "Unreadable name") == "Fusehive" &&
               PickerDisplayName("Magic Sound Effects\x15", "Unreadable name") == "Magic Sound Effects",
               "damaged trailing bytes must not appear in the picker")) return 1;
    if (!Check(PickerDisplayName("Fantasy Game Sounds\x01\x05vorbis+BCV", "Unreadable name") ==
                   "Fantasy Game Sounds" &&
               PickerDisplayName("\x1a\x1a\x1a", "Unreadable name") == "Unreadable name",
               "embedded tag bytes and control-only names must have readable labels")) return 1;
    if (!Check(PickerDisplayName("Boris Kar\xef\xbf\xbdkine", "Unreadable name") ==
                   "Boris Kar…kine" &&
               PickerDisplayName("Adele", "Unreadable name") == "Adele",
               "a damaged character must be marked without changing clean names")) return 1;

    const std::string damagedAlbum = "Robot Monster Vocalizations Vol. 3\x1a";
    MetadataPickerDraft albumDraft;
    albumDraft.Sync(damagedAlbum);
    if (!Check(albumDraft.InputDisplayText("Unreadable name") ==
                   "Robot Monster Vocalizations Vol. 3" &&
               albumDraft.Committed() == damagedAlbum,
               "an applied album shows a clean label while keeping its exact filter value")) return 1;
    albumDraft.Open();
    if (!Check(albumDraft.InputDisplayText("Unreadable name") ==
                   "Robot Monster Vocalizations Vol. 3",
               "opening a picker must not expose the damaged tag in its input")) return 1;
    albumDraft.Edit("Robot");
    if (!Check(albumDraft.InputDisplayText("Unreadable name") == "Robot" &&
               albumDraft.Committed() == damagedAlbum,
               "typing must display the query without changing the applied value")) return 1;
    albumDraft.Edit("");
    if (!Check(albumDraft.InputDisplayText("Unreadable name").empty(),
               "clearing the query must leave an empty input")) return 1;
    albumDraft.Cancel();
    if (!Check(albumDraft.InputDisplayText("Unreadable name") ==
                   "Robot Monster Vocalizations Vol. 3" &&
               albumDraft.Committed() == damagedAlbum,
               "closing without selection restores the readable applied label")) return 1;

    const SidebarPickerRect work{0.0f, 0.0f, 1280.0f, 900.0f};
    const SidebarPickerRect artistField{60.0f, 43.0f, 180.0f, 30.0f};
    const SidebarPickerRect tall = ComputePopupLayout(artistField, work);
    if (!Check(tall.x >= 240.0f && tall.width >= 550.0f && tall.height >= 840.0f &&
               tall.y + tall.height <= 894.0f,
               "a tall viewport must give the side list nearly all available height")) return 1;

    const SidebarPickerRect shortWork{0.0f, 0.0f, 420.0f, 360.0f};
    const SidebarPickerRect shortList = ComputePopupLayout(artistField, shortWork);
    if (!Check(shortList.x >= 6.0f && shortList.x + shortList.width <= 414.0f &&
               shortList.y + shortList.height <= 354.0f,
               "a narrow short viewport must keep the list on screen")) return 1;

    const SidebarPickerRect rightField{900.0f, 130.0f, 180.0f, 30.0f};
    const SidebarPickerRect leftList = ComputePopupLayout(rightField, work);
    if (!Check(leftList.x + leftList.width <= rightField.x - 8.0f,
               "a field near the right edge must place the list on its left")) return 1;

    std::puts("PASS: SidebarMetadataPicker");
    return 0;
}
