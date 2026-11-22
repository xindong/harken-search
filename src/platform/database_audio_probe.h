#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace Platform::DatabaseAudioProbe {

// @stage7-contract
// API contract: Probe reads the supplied audio path without mutating it and
// writes a fully-owned metadata snapshot to out on success. The caller owns
// path and out, must provide an existing readable file, and must call this
// synchronously from its worker/database context. False reports decode or IO
// failure and leaves the output value unspecified.

struct Info {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint64_t totalFrames = 0;
    float durationSeconds = 0.0f;
    std::optional<bool> wavPackLossless;
};

bool Probe(const std::filesystem::path& path, Info& out);

} // namespace Platform::DatabaseAudioProbe
