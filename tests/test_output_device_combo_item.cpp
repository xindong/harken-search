#include "OutputDeviceComboItem.h"

#include <cstdio>
#include <string>
#include <vector>

int main() {
    struct IdentifiedDevice { std::string name; std::vector<uint8_t> deviceIdBlob; };
    const std::vector<IdentifiedDevice> refreshed{{"Speakers",{2}},{"Speakers",{3}}};
    const std::vector<uint8_t> removedChoice{1};
    for(int retry=0;retry<2;++retry) {
        if(ResolveOutputDeviceIdentity(refreshed,0,"Speakers",removedChoice)!=-2) return 1;
    }
    const std::vector<IdentifiedDevice> reordered{{"B",{2}},{"A",{1}}};
    if(ResolveOutputDeviceIdentity(reordered,0,"A",{1})!=1 ||
       ResolveOutputDeviceIdentity(reordered,1,"B",{2})!=0 ||
       ResolveOutputDeviceIdentity(reordered,-1,"",{})!=-1) return 1;
    struct Device { std::string name; };
    const std::vector<Device> duplicates{{"Speakers"},{"Speakers"},{"Headphones"}};
    if (ResolveOutputDeviceSelection(duplicates,1,"Speakers") != 1 ||
        ResolveOutputDeviceSelection(duplicates,0,"Headphones") != 2 ||
        ResolveOutputDeviceSelection(duplicates,4,"Speakers") != -2 ||
        ResolveOutputDeviceSelection(duplicates,0,"Disconnected") != -2 ||
        ResolveOutputDeviceSelection(duplicates,-1,"") != -1) {
        std::printf("FAIL: staged output choice must survive re-enumeration without guessing among duplicate names\n");
        return 1;
    }
    const OutputDeviceComboItem a = MakeOutputDeviceComboItem(0, "Speakers", true, " (Default)");
    const OutputDeviceComboItem b = MakeOutputDeviceComboItem(1, "Speakers", false, " (Default)");

    if (a.displayLabel.find("Speakers") == std::string::npos) {
        std::printf("FAIL: default device display must keep the device name\n");
        return 1;
    }
    if (a.displayLabel.find(" (Default)") == std::string::npos) {
        std::printf("FAIL: default device display must append the supplied suffix\n");
        return 1;
    }
    if (b.displayLabel != "Speakers") {
        std::printf("FAIL: non-default display must stay the device name\n");
        return 1;
    }
    if (a.selectableId == b.selectableId) {
        std::printf("FAIL: same-named devices must get distinct Selectable IDs\n");
        return 1;
    }
    if (a.selectableId.find("##") == std::string::npos || b.selectableId.find("##") == std::string::npos) {
        std::printf("FAIL: Selectable IDs must use an explicit ImGui ## suffix\n");
        return 1;
    }

    // Two non-default devices with byte-identical names must render the same
    // label yet still get distinct Selectable IDs; only the index can tell them
    // apart, so this is the case that pins the fix's core scenario.
    const OutputDeviceComboItem c = MakeOutputDeviceComboItem(2, "Speakers", false, " (Default)");
    const OutputDeviceComboItem d = MakeOutputDeviceComboItem(3, "Speakers", false, " (Default)");
    if (c.displayLabel != d.displayLabel) {
        std::printf("FAIL: same-named non-default devices must share one display label\n");
        return 1;
    }
    if (c.selectableId == d.selectableId) {
        std::printf("FAIL: identical display labels must still get distinct Selectable IDs\n");
        return 1;
    }

    const char* firstCstr = a.displayLabel.c_str();
    const char* secondCstr = a.selectableId.c_str();
    if (std::string(firstCstr) != a.displayLabel || std::string(secondCstr) != a.selectableId) {
        std::printf("FAIL: combo strings must remain stable after c_str()\n");
        return 1;
    }

    std::printf("OK: output device combo items keep unique IDs and stable labels\n");
    return 0;
}
