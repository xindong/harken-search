#pragma once

#include "DawTarget.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

namespace harken::settings {

struct SettingsDawPreset {
    std::string_view id;
    const char* name;
    const char* processName;
    DawSendMode mode;

    DawTarget MakeTarget() const {
        DawTarget target;
        target.name = name;
        target.processName = processName;
        target.mode = mode;
        target.enabled = false;
        return target;
    }
};

inline constexpr std::array<SettingsDawPreset, 4> kSettingsDawPresets{{
    {"daw-reaper", "REAPER", "reaper.exe", DawSendMode::NamedPipe},
    {"daw-pro-tools", "Pro Tools", "ProTools.exe", DawSendMode::LaunchExe},
    {"daw-cubase", "Cubase", "Cubase14.exe", DawSendMode::LaunchExe},
    {"daw-nuendo", "Nuendo", "Nuendo.exe", DawSendMode::LaunchExe},
}};

inline size_t FindSettingsDawPresetIndex(std::string_view id) {
    for (size_t index = 0; index < kSettingsDawPresets.size(); ++index) {
        if (kSettingsDawPresets[index].id == id) return index;
    }
    return kSettingsDawPresets.size();
}

inline bool DawNamesEqual(std::string_view left, std::string_view right) {
    const auto lower = [](char ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; };
    return std::equal(left.begin(), left.end(), right.begin(), right.end(),
        [&](char lhs, char rhs) { return lower(lhs) == lower(rhs); });
}

template<class Targets>
auto FindSettingsDawTarget(Targets& targets, const SettingsDawPreset& preset) {
    return std::find_if(targets.begin(), targets.end(), [&](const DawTarget& target) {
        return DawNamesEqual(target.processName, preset.processName) ||
               DawNamesEqual(target.name, preset.name);
    });
}

class DawTargetDraft {
public:
    void Begin(const std::vector<DawTarget>& targets) {
        for (size_t index = 0; index < kSettingsDawPresets.size(); ++index) {
            const auto& preset = kSettingsDawPresets[index];
            const auto current = FindSettingsDawTarget(targets, preset);
            baseline_[index] = current == targets.end() ? preset.MakeTarget() : *current;
        }
        draft_ = baseline_;
    }

    auto& Value() { return draft_; }
    const auto& Value() const { return draft_; }

    size_t ChangedCount() const {
        size_t count = 0;
        for (size_t index = 0; index < draft_.size(); ++index) {
            count += draft_[index].enabled != baseline_[index].enabled;
            count += draft_[index].exePath != baseline_[index].exePath;
        }
        return count;
    }

    bool MergeInto(std::vector<DawTarget>& targets) const {
        size_t added = 0;
        for (size_t index = 0; index < draft_.size(); ++index) {
            if ((draft_[index].enabled != baseline_[index].enabled ||
                 draft_[index].exePath != baseline_[index].exePath) &&
                FindSettingsDawTarget(targets, kSettingsDawPresets[index]) == targets.end()) ++added;
        }
        if (added && targets.size() + added > 9) return false;

        for (size_t index = 0; index < draft_.size(); ++index) {
            const auto& edited = draft_[index];
            const bool enabledChanged = edited.enabled != baseline_[index].enabled;
            const bool pathChanged = edited.exePath != baseline_[index].exePath;
            if (!enabledChanged && !pathChanged) continue;
            const auto& preset = kSettingsDawPresets[index];
            auto current = FindSettingsDawTarget(targets, preset);
            if (current == targets.end()) {
                targets.push_back(preset.MakeTarget());
                current = targets.end() - 1;
            }
            if (enabledChanged) current->enabled = edited.enabled;
            if (pathChanged) {
                current->exePath = edited.exePath;
                current->lastDetectTime = -999.0;
            }
        }
        return true;
    }

private:
    std::array<DawTarget, kSettingsDawPresets.size()> baseline_{}, draft_{};
};

}
