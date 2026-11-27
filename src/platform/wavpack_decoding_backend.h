#pragma once

#include "../../thirdparty/miniaudio.h"
#include <optional>

namespace Platform::WavPack {

// @stage7-contract
// Applies the bundled WavPack decoder as a custom ma_decoder backend so .wv
// files decode through the same path as the built-in formats. The backend
// registers after the built-in decoders and rejects anything it cannot open,
// so existing formats keep their current behaviour.
void ApplyDecodingBackend(ma_decoder_config& config);
std::optional<bool> IsLossless(const ma_decoder& decoder);

#ifdef HARKEN_TEST_WAVPACK_VERIFICATION_COUNT
unsigned int CorrectionVerificationCountForTest();
#endif

}  // namespace Platform::WavPack
