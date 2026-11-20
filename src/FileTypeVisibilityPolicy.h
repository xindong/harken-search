#pragma once

#include <string>
#include <vector>

namespace aura::file_types {

struct FileTypeVisibility {
    bool showWav = true;
    bool showMp3 = true;
    bool showFlac = true;
    bool showOgg = true;
    bool showAiff = true;
    bool showM4a = true;
    bool showWma = true;
    bool showOther = true;
    bool showLosslessWavPack = false;
};

struct FileTypeVisibilityPolicy {
    bool allVisible = true;
    bool includeOnlyKnownExtensions = false;
    std::vector<std::string> excludedExtensions;
    bool losslessWavPackOnly = false;
};

FileTypeVisibilityPolicy BuildPolicy(const FileTypeVisibility& visibility);
bool isVisible(const FileTypeVisibility& visibility, const std::string& extension,
               bool wavPackLossless = false);

}  // namespace aura::file_types
