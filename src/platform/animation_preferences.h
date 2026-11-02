#pragma once

namespace Platform::Animation {
// @stage7-contract
// API contract: synchronously reads the OS client-animation preference, owns
// no state or handles, never changes preferences and never throws. False is
// returned when disabled, unsupported or unavailable; callers retain static
// status text and real progress counts in that case.
// Unknown/unsupported settings use static progress, never synthetic completion.
bool ClientAreaAnimationEnabled() noexcept;
}
