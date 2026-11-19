#pragma once

#include "../../thirdparty/miniaudio.h"

namespace Platform::WavPack {

// @stage7-contract
// Applies the bundled WavPack decoder as a custom ma_decoder backend so .wv
// files decode through the same path as the built-in formats. The backend
// registers after the built-in decoders and rejects anything it cannot open,
// so existing formats keep their current behaviour.
void ApplyDecodingBackend(ma_decoder_config& config);

}  // namespace Platform::WavPack
