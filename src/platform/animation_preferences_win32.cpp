#include "platform/animation_preferences.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace Platform::Animation {
bool ClientAreaAnimationEnabled() noexcept {
    BOOL enabled = FALSE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) && enabled;
}
}
#endif
