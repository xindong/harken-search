#include "src/FileTypeVisibilityPolicy.h"

#include <algorithm>
#include <cassert>

int main() {
    using namespace aura::file_types;
    FileTypeVisibility visibility;
    visibility.showWav = false;
    visibility.showOther = false;
    const auto policy = BuildPolicy(visibility);
    assert(!policy.allVisible);
    assert(policy.includeOnlyKnownExtensions);
    assert(policy.excludedExtensions.size() == 3);
    assert(!policy.losslessWavPackOnly);
    assert(!isVisible(visibility, ".wav"));
    assert(!isVisible(visibility, "WAVE"));
    assert(isVisible(visibility, ".mp3"));
    assert(!isVisible(visibility, ".unknown"));

    FileTypeVisibility lossless;
    lossless.showMp3 = lossless.showOgg = lossless.showM4a = false;
    lossless.showWma = lossless.showOther = false;
    lossless.showLosslessWavPack = true;
    const auto losslessPolicy = BuildPolicy(lossless);
    assert(losslessPolicy.losslessWavPackOnly);
    assert(losslessPolicy.includeOnlyKnownExtensions);
    assert(std::find(losslessPolicy.excludedExtensions.begin(),
                     losslessPolicy.excludedExtensions.end(), ".wv") ==
           losslessPolicy.excludedExtensions.end());
    assert(isVisible(lossless, ".wav"));
    assert(isVisible(lossless, ".flac"));
    assert(isVisible(lossless, ".aif"));
    assert(isVisible(lossless, ".WV", true));
    assert(isVisible(lossless, "wv", true));
    assert(!isVisible(lossless, ".WV", false));
    assert(!isVisible(lossless, ".wvc"));
    assert(!isVisible(lossless, ".mp3"));
    const FileTypeVisibility all;
    assert(isVisible(all, ".wv", true));
    assert(isVisible(all, ".WV", false));
    FileTypeVisibility wavOnly = lossless;
    wavOnly.showFlac = wavOnly.showAiff = wavOnly.showLosslessWavPack = false;
    assert(isVisible(wavOnly, ".WAVE"));
    assert(!isVisible(wavOnly, ".WV", true));
    return 0;
}
