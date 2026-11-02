#pragma once

#include <string>
#include <vector>

// -1 is the system default; -2 means the staged choice is no longer
// unambiguous. Preserve an unchanged index before falling back to its name.
template<class Device>
int ResolveOutputDeviceSelection(const std::vector<Device>& devices, int index, const std::string& name) {
    if (index < 0) return -1;
    if (index < static_cast<int>(devices.size()) && devices[index].name == name) return index;
    int found = -2;
    for (int i=0;i<static_cast<int>(devices.size());++i) if (devices[i].name == name) {
        if (found >= 0) return -2;
        found = i;
    }
    return found;
}

struct OutputDeviceComboItem {
    std::string displayLabel;
    std::string selectableId;
};

inline OutputDeviceComboItem MakeOutputDeviceComboItem(
    int index,
    const std::string& name,
    bool isDefault,
    const char* defaultSuffix)
{
    OutputDeviceComboItem item;
    item.displayLabel = name;
    if (isDefault && defaultSuffix != nullptr && defaultSuffix[0] != '\0') {
        item.displayLabel += defaultSuffix;
    }
    item.selectableId = item.displayLabel + "##output-device-" + std::to_string(index);
    return item;
}
