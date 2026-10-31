#pragma once

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
