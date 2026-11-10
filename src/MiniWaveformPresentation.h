#pragma once

#include <cstdlib>
#include <vector>

namespace MiniWaveformPresentation {

enum class Mode { Texture, CpuFallback, Placeholder };
enum class Removal { BudgetEviction, ExplicitInvalidation };

inline bool RetainTextureAfterRemoval(Removal removal) {
    return removal == Removal::BudgetEviction;
}

inline Mode Choose(bool dataReady, bool textureReady) {
    if (textureReady) return Mode::Texture;
    return dataReady ? Mode::CpuFallback : Mode::Placeholder;
}

// A texture with zero dimensions is a deliberately retained stale texture:
// keep displaying it until a replacement has finished uploading. This avoids
// a visible CPU/GPU presentation switch when a background waveform result
// refreshes an already visible row.
inline bool HasCompatibleTextureDimensions(bool hasTexture,
                                           int cachedWidth,
                                           int cachedHeight,
                                           int targetWidth,
                                           int targetHeight,
                                           int tolerance = 4) {
    if (!hasTexture || cachedWidth <= 0 || cachedHeight <= 0 ||
        targetWidth <= 0 || targetHeight <= 0) {
        return false;
    }
    return std::abs(cachedWidth - targetWidth) <= tolerance &&
           std::abs(cachedHeight - targetHeight) <= tolerance;
}

inline bool CanSeek(bool dataReady, bool textureReady) {
    return dataReady || textureReady;
}

class DeferredTextureDeletes {
public:
    void Queue(unsigned int& textureId) {
        if (textureId == 0) return;
        ids_.push_back(textureId);
        textureId = 0;
    }

    template <typename Delete>
    void Flush(Delete&& remove) {
        for (unsigned int id : ids_) remove(id);
        ids_.clear();
    }

private:
    std::vector<unsigned int> ids_;
};

} // namespace MiniWaveformPresentation
