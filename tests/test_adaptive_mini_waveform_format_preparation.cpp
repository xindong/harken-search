#include "src/AdaptiveMiniWaveformFormatPreparation.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

int Database::countFilesNeedingWaveformCache() {
    return 0;
}

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

class FakeSource final
    : public aura::waveform_rebuild::AdaptiveMiniFormatPreparationSource {
public:
    bool adaptive = false;
    int countError = 0;
    std::vector<std::vector<float>> filePeaks;
    mutable std::vector<std::string> calls;

    bool usesAdaptiveMiniWaveform() const override {
        calls.push_back("uses");
        return adaptive;
    }

    int countFilesNeedingWaveformCache() override {
        calls.push_back("count");
        if (countError < 0) {
            return countError;
        }
        return static_cast<int>(std::count_if(
            filePeaks.begin(), filePeaks.end(),
            [](const auto& peaks) { return peaks.empty(); }));
    }
};

}  // namespace

int main() {
    using aura::waveform_rebuild::AdaptiveMiniFormatFinalizationState;
    using aura::waveform_rebuild::AdaptiveMiniFormatPreparationDecision;
    using aura::waveform_rebuild::PrepareAdaptiveMiniWaveformFormat;

    FakeSource disabled;
    auto result = PrepareAdaptiveMiniWaveformFormat(
        disabled, AdaptiveMiniFormatFinalizationState{true, true});
    Check(result.decision == AdaptiveMiniFormatPreparationDecision::Disabled,
          "unsupported format returns disabled");
    Check(disabled.calls == std::vector<std::string>{"uses"},
          "unsupported format skips counting missing waveforms");
    Check(!result.state.rebuildPending && result.state.stampPending,
          "unsupported format clears only rebuild pending");

    const std::vector<float> savedPeaks(512, 0.625f);
    FakeSource cached;
    cached.adaptive = true;
    cached.filePeaks = {savedPeaks};
    result = PrepareAdaptiveMiniWaveformFormat(
        cached, AdaptiveMiniFormatFinalizationState{true, false});
    Check(cached.filePeaks.front() == savedPeaks,
          "cached WAV keeps its saved 512-point mini waveform");
    Check(result.decision == AdaptiveMiniFormatPreparationDecision::NoWork,
          "entirely cached library needs no rebuild");
    Check(cached.calls == std::vector<std::string>{"uses", "count"},
          "supported format only checks the format and counts missing waveforms");
    Check(result.rebuildTotal == 0 &&
              result.state.rebuildPending && !result.state.stampPending,
          "cached library reports zero missing waveforms and preserves pending state");

    FakeSource negative;
    negative.adaptive = true;
    negative.countError = -1;
    result = PrepareAdaptiveMiniWaveformFormat(
        negative, AdaptiveMiniFormatFinalizationState{false, true});
    Check(result.decision == AdaptiveMiniFormatPreparationDecision::NoWork,
          "negative debt preserves legacy no-work classification");
    Check(result.rebuildTotal == -1 &&
              !result.state.rebuildPending && result.state.stampPending,
          "negative debt preserves input state and exact telemetry");

    FakeSource rebuild;
    rebuild.adaptive = true;
    rebuild.filePeaks = {savedPeaks, {}};
    result = PrepareAdaptiveMiniWaveformFormat(
        rebuild, AdaptiveMiniFormatFinalizationState{true, false});
    Check(result.decision ==
              AdaptiveMiniFormatPreparationDecision::RebuildRequired,
          "missing waveform requests rebuild");
    Check(rebuild.filePeaks.front() == savedPeaks,
          "mixed library keeps its existing mini waveform");
    Check(result.rebuildTotal == 1 &&
              result.state.rebuildPending && !result.state.stampPending,
          "mixed library counts only the missing waveform and preserves pending state");

    std::printf("\n=== Adaptive Mini Waveform Format Preparation: %d failure(s) ===\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
