#pragma once

#include "AdaptiveMiniWaveformFormatFinalization.h"
#include "Database.h"

#include <cstdint>

namespace aura::waveform_rebuild {

enum class AdaptiveMiniFormatPreparationDecision {
    Disabled,
    NoWork,
    RebuildRequired,
};

struct AdaptiveMiniFormatPreparationResult {
    AdaptiveMiniFormatPreparationDecision decision =
        AdaptiveMiniFormatPreparationDecision::Disabled;
    AdaptiveMiniFormatFinalizationState state;
    int64_t rebuildTotal = 0;
};

class AdaptiveMiniFormatPreparationSource {
public:
    virtual ~AdaptiveMiniFormatPreparationSource() = default;

    virtual bool usesAdaptiveMiniWaveform() const = 0;
    virtual int countFilesNeedingWaveformCache() = 0;
};

class DatabaseAdaptiveMiniFormatPreparationSource final
    : public AdaptiveMiniFormatPreparationSource {
public:
    explicit DatabaseAdaptiveMiniFormatPreparationSource(
        Database& database) noexcept;

    bool usesAdaptiveMiniWaveform() const override;
    int countFilesNeedingWaveformCache() override;

private:
    Database& database_;
};

AdaptiveMiniFormatPreparationResult PrepareAdaptiveMiniWaveformFormat(
    AdaptiveMiniFormatPreparationSource& source,
    AdaptiveMiniFormatFinalizationState state);

}  // namespace aura::waveform_rebuild
