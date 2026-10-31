#include <cstdio>
#include <vector>

#include "src/MiniWaveformPresentation.h"

int main() {
    using MiniWaveformPresentation::Choose;
    using MiniWaveformPresentation::Mode;
    using MiniWaveformPresentation::Removal;
    using MiniWaveformPresentation::RetainTextureAfterRemoval;

    int failures = 0;
    const auto check = [&failures](bool condition, const char* label) {
        std::printf("%s: %s\n", condition ? "PASS" : "FAIL", label);
        if (!condition) ++failures;
    };

    check(Choose(false, true) == Mode::Texture,
          "loaded waveform stays visible while its CPU data is recycled");
    check(Choose(false, false) == Mode::Placeholder,
          "unloaded waveform without a texture shows the placeholder");
    check(Choose(true, false) == Mode::CpuFallback,
          "ready data without a texture can draw the waveform");
    check(Choose(true, true) == Mode::Texture,
          "ready data uses its texture");
    check(RetainTextureAfterRemoval(Removal::BudgetEviction),
          "memory budget eviction retains the bounded display texture");
    check(!RetainTextureAfterRemoval(Removal::ExplicitInvalidation),
          "file invalidation removes the old display texture");
    MiniWaveformPresentation::DeferredTextureDeletes deferred;
    unsigned int drawnTexture = 42;
    std::vector<unsigned int> deleted;
    deferred.Queue(drawnTexture);
    check(drawnTexture == 0 && deleted.empty(),
          "a texture referenced by the current frame survives until rendering ends");
    deferred.Flush([&](unsigned int id) { deleted.push_back(id); });
    check(deleted.size() == 1 && deleted.front() == 42,
          "old texture is deleted after the frame is rendered");
    check(MiniWaveformPresentation::CanSeek(false, true),
          "retained waveform remains clickable while CPU data reloads");
    check(!MiniWaveformPresentation::CanSeek(false, false),
          "placeholder does not enable waveform seeking");
    return failures == 0 ? 0 : 1;
}
