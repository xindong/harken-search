#include "AudioEngine.h"

#include <cmath>
#include <cstdio>

namespace {
bool Near(float actual, float expected) { return std::fabs(actual - expected) < 0.0001f; }
int Check(bool condition, const char* message) {
    if (condition) return 0;
    std::printf("FAIL: %s\n", message);
    return 1;
}
}

int main() {
    AudioEngine::TransferProcessingSnapshot requested;
    requested.pan = 0.45f;
    requested.centerSense = -0.3f;
    requested.bassMonoFreqHz = 90.0f;
    requested.mono = true;
    requested.reverse = true;
    requested.magicEQ.enabled = true;
    requested.magicEQ.centerFreq = 1500.0f;

    TransportEffectVisibility visibility;
    auto effective = ApplyTransportEffectVisibility(requested, visibility);
    if (Check(effective.pan == requested.pan && effective.centerSense == requested.centerSense &&
              effective.bassMonoFreqHz == requested.bassMonoFreqHz && effective.mono &&
              effective.reverse && effective.magicEQ.enabled, "default visibility preserves existing effects")) return 1;

    visibility.soundstage = false;
    effective = ApplyTransportEffectVisibility(requested, visibility);
    if (Check(Near(effective.centerSense, 0.0f) && Near(effective.bassMonoFreqHz, 0.0f) &&
              effective.magicEQ.enabled && Near(effective.pan, 0.45f), "soundstage bypasses CenterSense and BM together")) return 1;

    visibility.mono = false;
    visibility.reverse = false;
    visibility.pan = false;
    visibility.eq = false;
    effective = ApplyTransportEffectVisibility(requested, visibility);
    if (Check(!effective.mono && !effective.reverse && Near(effective.pan, 0.0f) &&
              Near(effective.centerSense, 0.0f) && Near(effective.bassMonoFreqHz, 0.0f) &&
              !effective.magicEQ.enabled, "all five hidden groups bypass independently")) return 1;
    if (Check(requested.mono && requested.reverse && Near(requested.pan, 0.45f) &&
              Near(requested.centerSense, -0.3f) && Near(requested.bassMonoFreqHz, 90.0f) &&
              requested.magicEQ.enabled && Near(requested.magicEQ.centerFreq, 1500.0f),
              "bypass does not erase requested parameters")) return 1;

    visibility = {};
    effective = ApplyTransportEffectVisibility(requested, visibility);
    if (Check(effective.mono && effective.reverse && Near(effective.pan, 0.45f) &&
              Near(effective.centerSense, -0.3f) && Near(effective.bassMonoFreqHz, 90.0f) &&
              effective.magicEQ.enabled, "showing groups restores requested state")) return 1;
    return 0;
}
