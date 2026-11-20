#include "FileTypeVisibilityPolicy.h"

#include <algorithm>
#include <cctype>

namespace aura::file_types {
namespace {

void AddExcluded(std::vector<std::string>& output,
                 bool visible,
                 std::initializer_list<const char*> extensions) {
    if (visible) return;
    for (const auto* extension : extensions) output.emplace_back(extension);
}

std::string Normalize(std::string extension) {
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
    return extension;
}

}  // namespace

FileTypeVisibilityPolicy BuildPolicy(const FileTypeVisibility& visibility) {
    FileTypeVisibilityPolicy policy;
    policy.allVisible = visibility.showWav && visibility.showMp3 &&
        visibility.showFlac && visibility.showOgg && visibility.showAiff &&
        visibility.showM4a && visibility.showWma && visibility.showOther;
    if (policy.allVisible) return policy;
    AddExcluded(policy.excludedExtensions, visibility.showWav, {".wav", ".wave"});
    AddExcluded(policy.excludedExtensions, visibility.showMp3, {".mp3"});
    AddExcluded(policy.excludedExtensions, visibility.showFlac, {".flac"});
    AddExcluded(policy.excludedExtensions, visibility.showOgg, {".ogg"});
    AddExcluded(policy.excludedExtensions, visibility.showAiff, {".aiff", ".aif"});
    AddExcluded(policy.excludedExtensions, visibility.showM4a, {".m4a"});
    AddExcluded(policy.excludedExtensions, visibility.showWma, {".wma"});
    AddExcluded(policy.excludedExtensions, visibility.showOther || visibility.showLosslessWavPack, {".wv"});
    policy.includeOnlyKnownExtensions = !visibility.showOther;
    policy.losslessWavPackOnly = !visibility.showOther && visibility.showLosslessWavPack;
    return policy;
}

bool isVisible(const FileTypeVisibility& visibility, const std::string& extension,
               bool wavPackLossless) {
    const std::string normalized = Normalize(extension);
    if (normalized == "wav" || normalized == "wave") return visibility.showWav;
    if (normalized == "mp3") return visibility.showMp3;
    if (normalized == "flac") return visibility.showFlac;
    if (normalized == "ogg") return visibility.showOgg;
    if (normalized == "aiff" || normalized == "aif") return visibility.showAiff;
    if (normalized == "m4a") return visibility.showM4a;
    if (normalized == "wma") return visibility.showWma;
    if (normalized == "wv") return visibility.showOther ||
        (visibility.showLosslessWavPack && wavPackLossless);
    return visibility.showOther;
}

}  // namespace aura::file_types
